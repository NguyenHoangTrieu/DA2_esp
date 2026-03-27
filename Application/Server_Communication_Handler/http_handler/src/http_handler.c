/**
 * @file http_handler.c
 * @brief HTTP/HTTPS telemetry publisher – ESP-IDF esp_http_client based.
 *
 * Receives pre-serialised JSON payloads via g_http_publish_queue and POSTs
 * them to the configured endpoint.  Token substitution (`{token}` inside
 * the URL) is performed automatically at publish time.
 */

#include "http_handler.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include "esp_crt_bundle.h"

static const char *TAG = "HTTP_HANDLER";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/** Live configuration — written by config_handler via http_handler_update_config() */
extern http_config_data_t g_http_cfg;
extern QueueHandle_t g_config_handler_queue;
extern config_type_t config_parse_type(const char *cmd, uint16_t len);

QueueHandle_t g_http_publish_queue = NULL;

static TaskHandle_t  s_task_handle        = NULL;
static TaskHandle_t  s_polling_task_handle = NULL;
static bool          s_task_running       = false;
static bool          s_polling_running    = false;
static int           s_last_rpc_id        = -1;

#define HTTP_QUEUE_SIZE 8
#define HTTP_CONTENT_TYPE "application/json"
#define HTTP_RPC_POLL_INTERVAL_MS 5000  // Poll every 5 seconds

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * Substitute `{token}` in the URL template with the actual auth token.
 * Writes result to `out` (max `out_size` bytes).
 */
static void build_url(const char *url_template, const char *token,
                      char *out, size_t out_size) {
    const char *placeholder = "{token}";
    const char *pos = strstr(url_template, placeholder);
    if (!pos) {
        strncpy(out, url_template, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    size_t before = pos - url_template;
    size_t tok_len = strlen(token);
    size_t after   = strlen(pos + strlen(placeholder));
    if (before + tok_len + after + 1 > out_size) {
        ESP_LOGE(TAG, "Composed URL too long, truncating");
        out_size--; // leave room for NUL
    }
    size_t written = 0;
    if (before < out_size - written - 1) {
        memcpy(out + written, url_template, before);
        written += before;
    }
    if (tok_len < out_size - written - 1) {
        memcpy(out + written, token, tok_len);
        written += tok_len;
    }
    size_t rem = out_size - written - 1;
    if (after > rem) after = rem;
    memcpy(out + written, pos + strlen(placeholder), after);
    written += after;
    out[written] = '\0';
}

/**
 * Perform a single HTTP POST of `payload` to the currently configured endpoint.
 * Returns ESP_OK on 2xx response, ESP_FAIL otherwise.
 */
static esp_err_t http_post_payload(const uint8_t *payload, size_t len) {
    char url[512];
    build_url(g_http_cfg.server_url, g_http_cfg.auth_token, url, sizeof(url));

    esp_http_client_config_t config = {
        .url                        = url,
        .method                     = HTTP_METHOD_POST,
        .timeout_ms                 = (int)g_http_cfg.timeout_ms,
        .crt_bundle_attach          = esp_crt_bundle_attach,   // <-- thêm dòng này
        .skip_cert_common_name_check = !g_http_cfg.verify_server,
        .transport_type             = g_http_cfg.use_tls
                                    ? HTTP_TRANSPORT_OVER_SSL
                                    : HTTP_TRANSPORT_OVER_TCP,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", HTTP_CONTENT_TYPE);
    esp_http_client_set_post_field(client, (const char *)payload, (int)len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "HTTP POST OK – status %d, %zu bytes sent", status, len);
            mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
        } else {
            ESP_LOGW(TAG, "HTTP POST returned status %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

/**
 * @brief Extract JSON params from HTTP RPC response and convert hex to binary
 */
static esp_err_t http_extract_rpc_params(const char *json_response, char *hex_out, size_t hex_out_size) {
    if (!json_response || !hex_out) return ESP_ERR_INVALID_ARG;
    
    // Look for "params" field in JSON: "params":"CF..." or "params":"hex_string"
    const char *params_start = strstr(json_response, "\"params\"");
    if (!params_start) {
        ESP_LOGW(TAG, "No params field in RPC response");
        return ESP_FAIL;
    }
    
    // Find opening quote after "params":
    params_start = strchr(params_start, '\"');
    if (!params_start) return ESP_FAIL;
    params_start++;  // Move past opening quote
    
    // Find closing quote
    const char *params_end = strchr(params_start, '\"');
    if (!params_end) return ESP_FAIL;
    
    size_t params_len = params_end - params_start;
    if (params_len >= hex_out_size) {
        ESP_LOGE(TAG, "RPC params too long (%zu > %zu)", params_len, hex_out_size);
        return ESP_FAIL;
    }
    
    memcpy(hex_out, params_start, params_len);
    hex_out[params_len] = '\0';
    return ESP_OK;
}

/**
 * @brief Poll for pending RPC commands from ThingsBoard
 */
static esp_err_t http_poll_rpc(void) {
    // Build polling URL: http://server:port/api/v1/{token}/rpc
    char url[512];
    build_url(g_http_cfg.server_url, g_http_cfg.auth_token, url, sizeof(url));
    
    // Replace /telemetry with /rpc for polling endpoint
    char *telemetry_pos = strstr(url, "/telemetry");
    if (telemetry_pos) {
        strcpy(telemetry_pos, "/rpc");
    } else {
        // If not /telemetry pattern, append /rpc
        strncat(url, "/rpc", sizeof(url) - strlen(url) - 1);
    }
    
    ESP_LOGI(TAG, "Polling RPC from: %s", url);
    
    esp_http_client_config_t config = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = (int)g_http_cfg.timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = !g_http_cfg.verify_server,
        .transport_type = g_http_cfg.use_tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP RPC poll client");
        return ESP_FAIL;
    }
    
    // Allocate buffer for response
    char *http_response = (char *)malloc(2048);
    if (!http_response) {
        ESP_LOGE(TAG, "Failed to allocate RPC response buffer");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    memset(http_response, 0, 2048);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    int content_read = 0;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP RPC connection: %s", esp_err_to_name(err));
        goto cleanup;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > 2048) {
        ESP_LOGW(TAG, "RPC response too large (%d > 2048)", content_length);
        content_length = 2048;
    }
    
    if (content_length > 0) {
        content_read = esp_http_client_read_response(client, http_response, content_length);
        
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET RPC response: status=%d, len=%d", status, content_read);
        
        if (status == 200 && content_read > 0) {
            ESP_LOGI(TAG, "RPC Response: %.*s", content_read > 100 ? 100 : content_read, (char *)http_response);
            
            // Extract RPC ID from response if present
            int rpc_id = -1;
            if (sscanf((const char *)http_response, "{\"id\":%d", &rpc_id) == 1) {
                s_last_rpc_id = rpc_id;
                ESP_LOGI(TAG, "Stored HTTP RPC ID: %d", s_last_rpc_id);
            }
            
            // Extract and parse command parameters
            char hex_string[512];
            if (http_extract_rpc_params((const char *)http_response, hex_string, sizeof(hex_string)) == ESP_OK) {
                ESP_LOGI(TAG, "Extracted RPC params: %s", hex_string);
                
                // Convert hex string to binary and enqueue to config handler
                uint8_t binary_cmd[256];
                size_t binary_len = 0;
                
                // Hex string to binary conversion
                for (size_t i = 0; i < strlen(hex_string) && binary_len < sizeof(binary_cmd); i += 2) {
                    if (sscanf(&hex_string[i], "%2hhx", &binary_cmd[binary_len]) == 1) {
                        binary_len++;
                    }
                }
                
                if (binary_len >= 2 && binary_cmd[0] == 'C' && binary_cmd[1] == 'F') {
                    ESP_LOGI(TAG, "Valid config command received via HTTP RPC");
                    
                    // Allocate command structure
                    config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
                    if (cmd) {
                        cmd->type = config_parse_type((const char *)binary_cmd, binary_len);
                        cmd->data_len = binary_len;
                        cmd->source = CMD_SOURCE_HTTP_RPC;
                        memcpy(cmd->raw_data, binary_cmd, binary_len);
                        cmd->raw_data[binary_len] = '\0';
                        
                        if (g_config_handler_queue) {
                            if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
                                ESP_LOGI(TAG, "HTTP RPC command enqueued to config handler");
                            } else {
                                ESP_LOGW(TAG, "Config queue full, dropping HTTP RPC");
                                free(cmd);
                            }
                        } else {
                            ESP_LOGW(TAG, "Config handler queue not initialized");
                            free(cmd);
                        }
                    }
                }
            }
        } else if (status == 204) {
            // 204 No Content - no pending RPC commands
            // This is normal
        }
    }
    
    err = ESP_OK;
    
cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(http_response);
    return err;
}

/**
 * @brief HTTP RPC polling task - periodically checks for pending commands
 */
static void http_polling_task(void *arg) {
    ESP_LOGI(TAG, "HTTP RPC polling task started, interval=%dms", HTTP_RPC_POLL_INTERVAL_MS);
    
    // Wait for first poll interval
    vTaskDelay(pdMS_TO_TICKS(HTTP_RPC_POLL_INTERVAL_MS));
    
    while (s_polling_running) {
        http_poll_rpc();
        vTaskDelay(pdMS_TO_TICKS(HTTP_RPC_POLL_INTERVAL_MS));
    }
    
    ESP_LOGI(TAG, "HTTP RPC polling task exiting");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void http_publish_task(void *arg) {
    http_publish_data_t item;
    uint8_t *data_buffer = (uint8_t *)malloc(1024);
    uint8_t *json_buffer = (uint8_t *)malloc(2048);
    char *hex_buffer = (char *)malloc(2048);
    
    if (!data_buffer || !json_buffer || !hex_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "HTTP publish task started");

    while (s_task_running) {
        if (xQueueReceive(g_http_publish_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (item.length == 0) continue;
            
            // Copy to local buffer
            size_t copy_len = (item.length < 1024) ? item.length : 1024;
            memcpy(data_buffer, item.data, copy_len);
            
            // Convert binary to hex string
            size_t hex_len = 0;
            for (size_t i = 0; i < copy_len && hex_len < 2048 - 3; i++) {
                hex_len += snprintf(&hex_buffer[hex_len], 2048 - hex_len, "%02X", data_buffer[i]);
            }
            
            // Wrap in JSON
            int json_len = snprintf((char *)json_buffer, 2048,
                                    "{\"data\":\"%s\"}",hex_buffer);
            
            ESP_LOGI(TAG, "Publishing %d bytes JSON (raw: %zu bytes → hex: %zu chars)",
                     json_len, copy_len, hex_len);
            
            // Check if this is an RPC response
            if (s_last_rpc_id >= 0) {
                // Route to RPC response endpoint
                char rpc_url[512];
                build_url(g_http_cfg.server_url, g_http_cfg.auth_token, rpc_url, sizeof(rpc_url));
                
                // Replace /telemetry with /rpc/response/{id}
                char *tel_pos = strstr(rpc_url, "/telemetry");
                if (tel_pos) {
                    snprintf(tel_pos, sizeof(rpc_url) - (tel_pos - rpc_url),
                             "/rpc/response/%d", s_last_rpc_id);
                }
                
                ESP_LOGI(TAG, "HTTP RPC response → %s", rpc_url);
                s_last_rpc_id = -1;  // Consume RPC ID
                
                // Send via HTTP POST to RPC response endpoint
                esp_http_client_config_t config = {
                    .url = rpc_url,
                    .method = HTTP_METHOD_POST,
                    .timeout_ms = (int)g_http_cfg.timeout_ms,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .skip_cert_common_name_check = !g_http_cfg.verify_server,
                    .transport_type = g_http_cfg.use_tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
                };
                
                esp_http_client_handle_t client = esp_http_client_init(&config);
                if (client) {
                    esp_http_client_set_header(client, "Content-Type", HTTP_CONTENT_TYPE);
                    esp_http_client_set_post_field(client, (const char *)json_buffer, json_len);
                    esp_err_t rc = esp_http_client_perform(client);
                    if (rc == ESP_OK) {
                        int status = esp_http_client_get_status_code(client);
                        ESP_LOGI(TAG, "HTTP RPC response sent: status=%d", status);
                    }
                    esp_http_client_cleanup(client);
                }
            } else {
                // Regular telemetry publish
                esp_err_t rc = http_post_payload(json_buffer, json_len);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG, "HTTP publish failed, payload dropped");
                }
            }
        }
    }

    ESP_LOGI(TAG, "HTTP publish task exiting");
    
cleanup:
    if (data_buffer) free(data_buffer);
    if (json_buffer) free(json_buffer);
    if (hex_buffer) free(hex_buffer);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void http_handler_task_start(void) {
    if (s_task_running) {
        ESP_LOGW(TAG, "HTTP handler already running");
        return;
    }

    if (!g_http_publish_queue) {
        g_http_publish_queue = xQueueCreate(HTTP_QUEUE_SIZE, sizeof(http_publish_data_t));
        if (!g_http_publish_queue) {
            ESP_LOGE(TAG, "Failed to create HTTP publish queue");
            return;
        }
    }

    s_task_running = true;
    xTaskCreate(http_publish_task, "http_publish", 8192, NULL, 5, &s_task_handle);
    ESP_LOGI(TAG, "HTTP handler task created");
    
    // Start polling task for RPC commands
    s_polling_running = true;
    xTaskCreate(http_polling_task, "http_poll_rpc", 4096, NULL, 4, &s_polling_task_handle);
    ESP_LOGI(TAG, "HTTP RPC polling task created");
}

void http_handler_task_stop(void) {
    s_task_running = false;
    s_task_handle = NULL;
    s_polling_running = false;
    s_polling_task_handle = NULL;
    ESP_LOGI(TAG, "HTTP handler tasks stopped");
}

bool http_enqueue_telemetry(const uint8_t *data, size_t data_len) {
    if (!data || data_len == 0 || !g_http_publish_queue) {
        return false;
    }

    if (data_len > HTTP_PUBLISH_DATA_MAX_LEN) {
        ESP_LOGW(TAG, "Payload too large (%zu > %d), truncating", data_len, HTTP_PUBLISH_DATA_MAX_LEN);
        data_len = HTTP_PUBLISH_DATA_MAX_LEN;
    }

    http_publish_data_t item;
    memcpy(item.data, data, data_len);
    item.length = data_len;

    if (xQueueSend(g_http_publish_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "HTTP publish queue full payload dropped");
        return false;
    }

    return true;
}

void http_handler_update_config(const http_config_data_t *cfg) {
    if (!cfg) return;
    // g_http_cfg is the live global; copy in new settings.
    // Note: ongoing requests in the task will use the new config on next iteration.
    memcpy(&g_http_cfg, cfg, sizeof(http_config_data_t));
    ESP_LOGI(TAG, "HTTP config updated URL: %s", g_http_cfg.server_url);
}
