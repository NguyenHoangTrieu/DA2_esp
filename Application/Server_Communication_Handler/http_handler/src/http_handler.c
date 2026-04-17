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
// In-flight guard: set when a command is forwarded, cleared on successful POST response.
static int           s_processing_rpc_id  = -1;
// Cooldown guard: after a response is accepted (200/406), the same id is suppressed for
// RPC_ID_COOLDOWN_TICKS ticks so that async ThingsBoard lag doesn't cause re-forwarding.
static int           s_cooldown_rpc_id    = -1;
static uint32_t      s_cooldown_rpc_ts    =  0;  // xTaskGetTickCount() snapshot
#define RPC_ID_COOLDOWN_TICKS pdMS_TO_TICKS(15000)  // 15-second cooldown after completion

#define HTTP_QUEUE_SIZE 8
#define HTTP_CONTENT_TYPE "application/json"

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
 * Detect URL scheme and return appropriate transport type.
 * Returns HTTP_TRANSPORT_OVER_SSL for https://, HTTP_TRANSPORT_OVER_TCP for http://
 */
static esp_http_client_transport_t detect_transport_from_url(const char *url) {
    if (strncmp(url, "https://", 8) == 0) {
        return HTTP_TRANSPORT_OVER_SSL;
    }
    return HTTP_TRANSPORT_OVER_TCP;
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
        .crt_bundle_attach          = g_http_cfg.verify_server ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = !g_http_cfg.verify_server,
        .transport_type             = detect_transport_from_url(url),
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
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
    }

    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

/**
 * @brief Extract JSON params from HTTP RPC response and convert hex to binary.
 *
 * JSON from ThingsBoard: {"id":1,"method":"sendCommand","params":"4346..."}
 * We need to skip past "params": to reach the VALUE string.
 */
static esp_err_t http_extract_rpc_params(const char *json_response, char *hex_out, size_t hex_out_size) {
    if (!json_response || !hex_out) return ESP_ERR_INVALID_ARG;

    // Step 1: find the key
    const char *p = strstr(json_response, "\"params\"");
    if (!p) {
        ESP_LOGW(TAG, "No params field in RPC response");
        return ESP_FAIL;
    }

    // Step 2: skip the 8-char key literal "params" to reach ':'
    p += 8;  // now pointing at '"' that CLOSES the key, i.e. '":...'
    p = strchr(p, ':');
    if (!p) return ESP_FAIL;
    p++;  // skip ':'

    // Step 3: skip optional whitespace, then the opening '"' of the VALUE
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') {
        ESP_LOGW(TAG, "params value is not a string (got '%c')", *p);
        return ESP_FAIL;
    }
    p++;  // skip opening '"'

    // Step 4: extract until the closing '"'
    const char *params_end = strchr(p, '"');
    if (!params_end) return ESP_FAIL;

    size_t params_len = params_end - p;
    if (params_len >= hex_out_size) {
        ESP_LOGE(TAG, "RPC params too long (%zu > %zu)", params_len, hex_out_size);
        return ESP_FAIL;
    }

    memcpy(hex_out, p, params_len);
    hex_out[params_len] = '\0';
    ESP_LOGI(TAG, "Extracted params (%zu chars): %.40s%s", params_len, hex_out, params_len > 40 ? "..." : "");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP response capture context (used by event handler during perform())
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    int   buf_size;
    int   offset;
} http_resp_ctx_t;

static esp_err_t rpc_http_event_handler(esp_http_client_event_t *evt) {
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data && evt->data_len > 0) {
        if (!esp_http_client_is_chunked_response(evt->client)) {
            int available = ctx->buf_size - ctx->offset - 1;
            int to_copy   = (evt->data_len < available) ? evt->data_len : available;
            if (to_copy > 0) {
                memcpy(ctx->buf + ctx->offset, evt->data, to_copy);
                ctx->offset += to_copy;
            }
        }
    }
    return ESP_OK;
}

/**
 * @brief Poll for pending RPC commands from ThingsBoard.
 *        Uses long-polling: server holds the connection open for up to 20 s.
 */
static esp_err_t http_poll_rpc(void) {
    char url[512];
    build_url(g_http_cfg.server_url, g_http_cfg.auth_token, url, sizeof(url));

    // Replace /telemetry with /rpc?timeout=20000 for polling endpoint
    char *telemetry_pos = strstr(url, "/telemetry");
    if (telemetry_pos) {
        strcpy(telemetry_pos, "/rpc?timeout=20000");
    } else {
        strncat(url, "/rpc?timeout=20000", sizeof(url) - strlen(url) - 1);
    }

    // Allocate response capture buffer
    char *http_response = (char *)calloc(2048, 1);
    if (!http_response) {
        ESP_LOGE(TAG, "Failed to allocate RPC response buffer");
        return ESP_FAIL;
    }

    http_resp_ctx_t resp_ctx = {
        .buf      = http_response,
        .buf_size = 2048,
        .offset   = 0,
    };

    esp_http_client_config_t config = {
        .url                       = url,
        .method                    = HTTP_METHOD_GET,
        .timeout_ms                = 22000,  // > server long-poll (20 000 ms)
        .crt_bundle_attach         = g_http_cfg.verify_server ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = !g_http_cfg.verify_server,
        .transport_type            = detect_transport_from_url(url),
        .buffer_size               = 2048,
        .buffer_size_tx            = 512,
        .event_handler             = rpc_http_event_handler,  // capture body here
        .user_data                 = &resp_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP RPC poll client");
        free(http_response);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to perform HTTP RPC GET: %s", esp_err_to_name(err));
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);  // Server unreachable
        goto cleanup;
    }

    // Response body was captured by event handler into http_response
    http_response[resp_ctx.offset] = '\0';
    int bytes_read = resp_ctx.offset;

    int status = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP RPC: status=%d, body_len=%d", status, bytes_read);
    if (bytes_read > 0) {
        ESP_LOGD(TAG, "RPC Body: %.*s", bytes_read > 120 ? 120 : bytes_read, http_response);
    }

    if (status == 200 && bytes_read > 0) {
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);

        // Extract RPC ID from response
        int rpc_id = -1;
        if (sscanf(http_response, "{\"id\":%d", &rpc_id) == 1) {
            ESP_LOGI(TAG, "Poll: RPC id=%d (in-flight=%d, cooldown=%d)",
                     rpc_id, s_processing_rpc_id, s_cooldown_rpc_id);

            if (rpc_id == s_processing_rpc_id) {
                // Still being processed downstream — don't forward again.
                ESP_LOGI(TAG, "RPC id=%d in-flight, skipping duplicate poll", rpc_id);
                err = ESP_OK;
                goto cleanup;
            }

            if (rpc_id == s_cooldown_rpc_id) {
                uint32_t elapsed = xTaskGetTickCount() - s_cooldown_rpc_ts;
                if (elapsed < RPC_ID_COOLDOWN_TICKS) {
                    ESP_LOGI(TAG, "RPC id=%d in cooldown (%lu ms / %lu ms window), skipping",
                             rpc_id,
                             (unsigned long)(elapsed * portTICK_PERIOD_MS),
                             (unsigned long)(RPC_ID_COOLDOWN_TICKS * portTICK_PERIOD_MS));
                    err = ESP_OK;
                    goto cleanup;
                } else {
                    ESP_LOGI(TAG, "RPC id=%d cooldown expired, treating as new", rpc_id);
                    s_cooldown_rpc_id = -1;
                }
            }

            // New command: mark as in-flight and store for response routing.
            s_processing_rpc_id = rpc_id;
            s_last_rpc_id       = rpc_id;

            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            // Extract and forward ONLY for genuinely new commands.
            // Everything below MUST stay inside this if(sscanf) block so
            // that dedup goto-cleanup above correctly short-circuits it.
            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            char hex_string[512];
            if (http_extract_rpc_params(http_response, hex_string, sizeof(hex_string)) == ESP_OK) {

                // Convert hex string to binary
                uint8_t binary_cmd[256];
                size_t binary_len = 0;
                for (size_t i = 0; i < strlen(hex_string) && binary_len < sizeof(binary_cmd); i += 2) {
                    if (sscanf(&hex_string[i], "%2hhx", &binary_cmd[binary_len]) == 1) {
                        binary_len++;
                    }
                }

                // Remap CFML:... → ML:... so config_parse_type() recognises it
                uint8_t *enqueue_cmd = binary_cmd;
                size_t   enqueue_len = binary_len;
                uint8_t  remapped[256];

                if (binary_len >= 4 &&
                    binary_cmd[0] == 'C' && binary_cmd[1] == 'F' &&
                    binary_cmd[2] == 'M' && binary_cmd[3] == 'L') {
                    enqueue_len = binary_len - 2;
                    memcpy(remapped, binary_cmd + 2, enqueue_len);
                    remapped[enqueue_len] = '\0';
                    enqueue_cmd = remapped;
                    ESP_LOGI(TAG, "Remapped CFML→ML: %.*s", (int)enqueue_len, enqueue_cmd);
                }

                config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
                if (cmd) {
                    cmd->type     = config_parse_type((const char *)enqueue_cmd, enqueue_len);
                    cmd->data_len = enqueue_len;
                    cmd->source   = CMD_SOURCE_HTTP_RPC;
                    memcpy(cmd->raw_data, enqueue_cmd, enqueue_len);
                    cmd->raw_data[enqueue_len] = '\0';

                    if (cmd->type == CONFIG_TYPE_UNKNOWN) {
                        ESP_LOGW(TAG, "HTTP RPC: unrecognised command type after remap, dropping");
                        free(cmd);
                    } else if (g_config_handler_queue) {
                        if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
                            ESP_LOGI(TAG, "HTTP RPC command enqueued (type=%d, len=%zu)", cmd->type, enqueue_len);
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
        } else {
            ESP_LOGW(TAG, "HTTP RPC body has no parseable id field");
        }
    } else if (status == 200 && bytes_read == 0) {
        // Server returned 200 but body was empty — unusual, still mark online
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
        ESP_LOGW(TAG, "HTTP RPC status 200 but empty body");
    } else if (status == 204) {
        // 204 No Content — no pending RPC commands (normal heartbeat)
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
        ESP_LOGD(TAG, "No pending RPC commands (status 204)");
    } else {
        ESP_LOGW(TAG, "HTTP RPC unexpected status: %d", status);
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
    }

    err = ESP_OK;

cleanup:
    esp_http_client_cleanup(client);
    free(http_response);
    return err;
}

/**
 * @brief HTTP RPC polling task - periodically checks for pending commands
 */
static void http_polling_task(void *arg) {
    ESP_LOGI(TAG, "HTTP RPC polling task started (continuous long-poll mode)");

    // Small initial delay to let WiFi/IP stack stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (s_polling_running) {
        http_poll_rpc();
        // Minimal gap (100ms) just to yield to other tasks between polls.
        // The 20s long-poll itself is the rate limiter, not this delay.
        vTaskDelay(pdMS_TO_TICKS(100));
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
                // -------------------------------------------------------
                // ThingsBoard HTTP device-side RPC response endpoint:
                //   POST /api/v1/{token}/rpc/{requestId}      ✅ correct
                //   POST /api/v1/{token}/rpc/response/{id}    ❌ wrong → 404
                // -------------------------------------------------------
                char rpc_url[512];
                build_url(g_http_cfg.server_url, g_http_cfg.auth_token, rpc_url, sizeof(rpc_url));

                char *tel_pos = strstr(rpc_url, "/telemetry");
                if (tel_pos) {
                    snprintf(tel_pos, sizeof(rpc_url) - (tel_pos - rpc_url),
                             "/rpc/%d", s_last_rpc_id);   // ✅ /rpc/{id}
                }

                // Build a human-readable result from the raw uplink bytes.
                // Format: "BLE"(3) + "DD/MM/YYYY-HH:MM:SS"(19) + <payload>
                // Strip the 22-byte prefix so widget gets the payload directly.
                char result_str[1024];  // must hold full 1024-byte data_buffer
                size_t rs_len = (copy_len < sizeof(result_str) - 1)
                                ? copy_len : sizeof(result_str) - 1;
                memcpy(result_str, data_buffer, rs_len);
                result_str[rs_len] = '\0';

                const char *payload_start = result_str;
                size_t payload_len = rs_len;
                if (rs_len > 22 &&
                    result_str[0] == 'B' && result_str[1] == 'L' && result_str[2] == 'E' &&
                    result_str[5] == '/' && result_str[13] == '-') {
                    payload_start = result_str + 22;
                    payload_len   = rs_len - 22;
                }

                // JSON-escape the payload so the full content (including 0x1E-delimited
                // +SCAN lines) is delivered to the widget.
                //   0x1E record separator → \n  (valid JSON; widget splits on \n)
                //   '"'  →  \"              (valid JSON escape)
                //   '\'  →  \\             (valid JSON escape)
                //   other control chars (<0x20) → skipped (invalid in JSON strings)
                char *esc = (char *)malloc(payload_len * 2 + 4);  // worst-case expansion
                if (!esc) {
                    ESP_LOGE(TAG, "malloc for JSON escape buffer failed");
                    goto cleanup;
                }
                size_t esc_len = 0;
                for (size_t ci = 0; ci < payload_len; ci++) {
                    unsigned char c = (unsigned char)payload_start[ci];
                    if (c == 0x1E) {
                        esc[esc_len++] = '\\';
                        esc[esc_len++] = 'n';   // \n — widget splitResp splits on \n
                    } else if (c == '"') {
                        esc[esc_len++] = '\\';
                        esc[esc_len++] = '"';
                    } else if (c == '\\') {
                        esc[esc_len++] = '\\';
                        esc[esc_len++] = '\\';
                    } else if (c >= 0x20) {
                        esc[esc_len++] = (char)c;
                    }
                    // else: skip other control characters
                }
                esc[esc_len] = '\0';

                char resp_json[2300];
                int resp_json_len = snprintf(resp_json, sizeof(resp_json),
                                             "{\"result\":\"%s\"}", esc);
                free(esc);

                ESP_LOGI(TAG, "HTTP RPC response → %s  body=%s", rpc_url, resp_json);
                s_last_rpc_id = -1;  // Consume RPC ID before the HTTP call

                esp_http_client_config_t config = {
                    .url            = rpc_url,
                    .method         = HTTP_METHOD_POST,
                    .timeout_ms     = (int)g_http_cfg.timeout_ms,
                    .crt_bundle_attach         = g_http_cfg.verify_server ? esp_crt_bundle_attach : NULL,
                    .skip_cert_common_name_check = !g_http_cfg.verify_server,
                    .transport_type = detect_transport_from_url(rpc_url),
                };

                esp_http_client_handle_t client = esp_http_client_init(&config);
                if (client) {
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_set_post_field(client, resp_json, resp_json_len);
                    esp_err_t rc = esp_http_client_perform(client);
                    if (rc == ESP_OK) {
                        int status_resp = esp_http_client_get_status_code(client);
                        ESP_LOGI(TAG, "HTTP RPC response sent: status=%d", status_resp);
                        if (status_resp == 200 || status_resp == 406) {
                            // 200 = accepted,  406 = ThingsBoard already has a response
                            // (can happen on race-condition retry). Both mean we're done.
                            if (status_resp == 406) {
                                ESP_LOGW(TAG, "ThingsBoard 406: RPC already has a response (race)");
                            } else {
                                ESP_LOGI(TAG, "ThingsBoard acknowledged RPC response \u2713");
                            }
                            // Enter cooldown so that stale polls for this id are dropped.
                            s_cooldown_rpc_id = s_processing_rpc_id >= 0
                                                ? s_processing_rpc_id
                                                : (int)s_last_rpc_id;
                            s_cooldown_rpc_ts = xTaskGetTickCount();
                            s_processing_rpc_id = -1;  // Release in-flight guard
                        } else {
                            ESP_LOGW(TAG, "ThingsBoard unexpected RPC response status: %d", status_resp);
                        }
                    } else {
                        ESP_LOGE(TAG, "HTTP RPC response POST failed: %s", esp_err_to_name(rc));
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
    {
        StackType_t *pub_stack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *pub_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (pub_stack && pub_tcb) {
            s_task_handle = xTaskCreateStatic(http_publish_task, "http_publish", 8192, NULL, 5, pub_stack, pub_tcb);
            ESP_LOGI(TAG, "HTTP handler task created in PSRAM");
        } else {
            ESP_LOGE(TAG, "Failed to allocate http_publish task in PSRAM");
        }
    }
    
    // Start polling task for RPC commands
    s_polling_running = true;
    {
        StackType_t *poll_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *poll_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (poll_stack && poll_tcb) {
            s_polling_task_handle = xTaskCreateStatic(http_polling_task, "http_poll_rpc", 4096, NULL, 4, poll_stack, poll_tcb);
            ESP_LOGI(TAG, "HTTP RPC polling task created in PSRAM");
        } else {
            ESP_LOGE(TAG, "Failed to allocate http_poll_rpc task in PSRAM");
        }
    }
}

void http_handler_task_stop(void) {
    s_task_running = false;
    s_task_handle = NULL;
    s_polling_running = false;
    s_polling_task_handle = NULL;
    if (g_http_publish_queue) {
        vQueueDelete(g_http_publish_queue);
        g_http_publish_queue = NULL;
    }
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
