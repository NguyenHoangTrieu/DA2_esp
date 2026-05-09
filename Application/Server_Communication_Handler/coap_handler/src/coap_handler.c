/**
 * @file coap_handler.c
 * @brief CoAP client telemetry publisher using ESP-IDF libcoap (coap3).
 */

#include "coap_handler.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Include libcoap header provided by ESP-IDF component */
#include "coap3/coap.h"

#include <lwip/netdb.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "COAP_HANDLER";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/** Live configuration — written by config_handler via coap_handler_update_config() */
extern coap_config_data_t g_coap_cfg;
extern QueueHandle_t g_config_handler_queue;
extern config_type_t config_parse_type(const char *cmd, uint16_t len);

QueueHandle_t g_coap_publish_queue = NULL;

static TaskHandle_t  s_task_handle = NULL;
static TaskHandle_t  s_polling_task_handle = NULL;
static bool          s_task_running = false;
static bool          s_polling_running = false;
static volatile bool s_coap_server_connected = false;
static int           s_last_rpc_id = -1;
static StaticQueue_t *s_coap_publish_qcb = NULL;
static uint8_t *s_coap_publish_qstorage = NULL;

/* RPC payload staging buffer — written by response handler, read by polling task */
static char          s_rpc_payload_buf[512];
static volatile bool s_rpc_payload_ready = false;

#define COAP_QUEUE_SIZE 32
#define COAP_ENQUEUE_WAIT_MS 250
#define COAP_ACK_TIMEOUT_DEFAULT_MS 2000
#define COAP_WAIT_TICKS pdMS_TO_TICKS(5000)
#define COAP_RPC_POLL_INTERVAL_DEFAULT_MS 200  // Default poll interval (reduced from 1500ms)
#define COAP_POST_PUBLISH_DELAY_MS 0
#define COAP_JSON_PAYLOAD_CAP ((COAP_PUBLISH_DATA_MAX_LEN * 2) + 128)

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * Substitute `{token}` in the resource path template with the actual token.
 */
static void build_resource_path(const char *path_template, const char *token,
                                 char *out, size_t out_size) {
    const char *placeholder = "{token}";
    const char *pos = strstr(path_template, placeholder);
    if (!pos) {
        strncpy(out, path_template, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    size_t before  = pos - path_template;
    size_t tok_len = strlen(token);
    size_t after   = strlen(pos + strlen(placeholder));
    size_t total   = before + tok_len + after + 1;

    if (total > out_size) {
        ESP_LOGE(TAG, "Resource path too long, truncating");
        total = out_size;
    }

    size_t w = 0;
    size_t copy_before = before < out_size - 1 ? before : out_size - 1;
    memcpy(out + w, path_template, copy_before); w += copy_before;
    size_t copy_tok = tok_len < out_size - w - 1 ? tok_len : out_size - w - 1;
    memcpy(out + w, token, copy_tok); w += copy_tok;
    size_t rem = out_size - w - 1;
    size_t copy_after = after < rem ? after : rem;
    memcpy(out + w, pos + strlen(placeholder), copy_after); w += copy_after;
    out[w] = '\0';
}

/**
 * Resolve hostname to a coap_address_t using lwIP getaddrinfo.
 * Returns ESP_OK on success.
 */
static esp_err_t resolve_host(const char *host, uint16_t port,
                               coap_address_t *dst) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolution failed for %s: %d", host, rc);
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
        return ESP_FAIL;
    }

    coap_address_init(dst);
    memcpy(&dst->addr.sin, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return ESP_OK;
}

/**
* @brief CoAP response handler
*/
static coap_response_t coap_response_handler(coap_session_t *s,
                                              const coap_pdu_t *sent,
                                              const coap_pdu_t *recv,
                                              const int mid) {
    coap_pdu_code_t code = coap_pdu_get_code(recv);
    s_coap_server_connected = true;
    if (COAP_RESPONSE_CLASS(code) != 2) {
        ESP_LOGE(TAG, "CoAP error %d.%02d MID=%d",
                 COAP_RESPONSE_CLASS(code), code & 0x1F, mid);
    } else {
        ESP_LOGI(TAG, "CoAP ACK %d.%02d MID=%d",
                 COAP_RESPONSE_CLASS(code), code & 0x1F, mid);
        
        /* Set internet status on successful CoAP response (polling or publish) */
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
        
        /* Try to extract payload — may be an RPC Observe notification */
        size_t data_len = 0;
        const uint8_t *data = NULL;
        if (coap_get_data(recv, &data_len, &data) && data_len > 0) {
            if (data_len < sizeof(s_rpc_payload_buf) - 1) {
                memcpy(s_rpc_payload_buf, data, data_len);
                s_rpc_payload_buf[data_len] = '\0';
                s_rpc_payload_ready = true;
                ESP_LOGI(TAG, "CoAP RPC payload received: %.*s", (int)data_len, data);
            } else {
                ESP_LOGW(TAG, "CoAP RPC payload too large (%zu bytes), ignoring", data_len);
            }
        }
    }

    /* Signal coap_post_to_resource I/O loop to stop (only set for publish contexts) */
    coap_context_t *resp_ctx = coap_session_get_context(s);
    void *app = coap_context_get_app_data(resp_ctx);
    if (app) *(bool *)app = true;

    return COAP_RESPONSE_OK;
}

/**
* @brief CoAP NACK handler
*/
static void coap_nack_handler(coap_session_t *s, const coap_pdu_t *sent,
                               coap_nack_reason_t reason, coap_mid_t mid) {
    ESP_LOGE(TAG, "CoAP NACK reason=%d MID=%d", reason, mid);
    s_coap_server_connected = false;
    /* Also signal publish context to stop waiting */
    coap_context_t *nack_ctx = coap_session_get_context(s);
    void *app = coap_context_get_app_data(nack_ctx);
    if (app) *(bool *)app = true;
}

/**
 * Send a single CoAP PUT (Confirmable) with the given payload.
 */
static esp_err_t coap_post_to_resource(const char *resource_path, const uint8_t *payload, size_t len) {
    /* Resolve address */
    coap_address_t dst;
    esp_err_t err = resolve_host(g_coap_cfg.host, g_coap_cfg.port, &dst);
    if (err != ESP_OK) return err;

    /* Create CoAP context */
    coap_context_t *ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to create CoAP context");
        return ESP_FAIL;
    }
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP);
    coap_register_response_handler(ctx, coap_response_handler);
    coap_register_nack_handler(ctx, coap_nack_handler);
    /* Create session */
    coap_session_t *session = NULL;
    if (g_coap_cfg.use_dtls) {
        coap_dtls_cpsk_t psk = {
            .version = COAP_DTLS_CPSK_SETUP_VERSION,
            .client_sni = g_coap_cfg.host,
            .psk_info = {
                .identity = { .s   = (uint8_t *)g_coap_cfg.device_token,
                            .length = strlen(g_coap_cfg.device_token) },
                .key      = { .s   = (uint8_t *)g_coap_cfg.device_token,
                            .length = strlen(g_coap_cfg.device_token) },
            },
        };
        session = coap_new_client_session_psk2(ctx, NULL, &dst, COAP_PROTO_DTLS, &psk);
    } else {
        session = coap_new_client_session(ctx, NULL, &dst, COAP_PROTO_UDP);
    }
    if (!session) {
        ESP_LOGE(TAG, "Failed to create CoAP session to %s:%u", g_coap_cfg.host, g_coap_cfg.port);
        coap_free_context(ctx);
        return ESP_FAIL;
    }

    /* Build URI option string: split path into path segments */
    coap_pdu_t *pdu = coap_new_pdu(COAP_MESSAGE_CON, COAP_REQUEST_CODE_POST, session);
    if (!pdu) {
        ESP_LOGE(TAG, "Failed to allocate CoAP PDU");
        coap_session_release(session);
        coap_free_context(ctx);
        return ESP_FAIL;
    }

    /* Add Uri-Path options by splitting resource_path on '/' */
    {
        char path_copy[256];
        strncpy(path_copy, resource_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';
        char *segment = strtok(path_copy, "/");
        while (segment) {
            coap_add_option(pdu, COAP_OPTION_URI_PATH,
                            strlen(segment), (const uint8_t *)segment);
            segment = strtok(NULL, "/");
        }
    }

    /* Content-Format: application/json = 50 */
    static const uint8_t json_fmt[] = { 50 };
    coap_add_option(pdu, COAP_OPTION_CONTENT_FORMAT, sizeof(json_fmt), json_fmt);

    /* Payload */
    coap_add_data(pdu, len, payload);

    /* Register a per-context flag so the response/NACK handler breaks the loop early */
    bool response_received = false;
    coap_context_set_app_data(ctx, &response_received);

    /* Send */
    coap_mid_t mid = coap_send(session, pdu);
    if (mid == COAP_INVALID_MID) {
        ESP_LOGE(TAG, "CoAP send failed");
        s_coap_server_connected = false;
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
        coap_session_release(session);
        coap_free_context(ctx);
        return ESP_FAIL;
    }

    /* Pump I/O loop until ACK/error received (early-break) or absolute timeout */
    uint32_t ack_timeout_ms = g_coap_cfg.ack_timeout_ms > 0
                                  ? g_coap_cfg.ack_timeout_ms
                                  : COAP_ACK_TIMEOUT_DEFAULT_MS;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ack_timeout_ms * 2);

    while (xTaskGetTickCount() < deadline) {
        int io_ms = coap_io_process(ctx, 100 /* ms */);
        if (io_ms < 0) break;
        if (response_received) break;  /* Response/NACK received — no need to wait longer */
    }

    if (!response_received) {
        s_coap_server_connected = false;
    }

    ESP_LOGI(TAG, "CoAP POST sent MID %d, path: %s, %zu bytes", mid, resource_path, len);
    mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);

    coap_session_release(session);
    coap_free_context(ctx);
    return ESP_OK;
}

/**
 * @brief CoAP RPC polling - GET pending commands from CoAP server
 */
static esp_err_t coap_poll_rpc(void) {
    char resource_path[256];
    build_resource_path(g_coap_cfg.resource_path, g_coap_cfg.device_token,
                        resource_path, sizeof(resource_path));
    
    // Replace /telemetry with /rpc for RPC endpoint
    char *telem_pos = strstr(resource_path, "/telemetry");
    if (telem_pos) {
        strcpy(telem_pos, "/rpc");  // Replace /telemetry with /rpc
    } else {
        // If no /telemetry in path, just append /rpc
        strncat(resource_path, "/rpc", sizeof(resource_path) - strlen(resource_path) - 1);
    }
    ESP_LOGD(TAG, "Polling RPC from: %s:%u%s", g_coap_cfg.host, g_coap_cfg.port, resource_path);
    
    coap_address_t dst;
    esp_err_t err = resolve_host(g_coap_cfg.host, g_coap_cfg.port, &dst);
    if (err != ESP_OK) return err;
    
    /* Create CoAP context */
    coap_context_t *ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to create CoAP context for polling");
        return ESP_FAIL;
    }
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP);
    coap_register_response_handler(ctx, coap_response_handler);
    
    /* Create session */
    coap_session_t *session = NULL;
    if (g_coap_cfg.use_dtls) {
        coap_dtls_cpsk_t psk = {
            .version = COAP_DTLS_CPSK_SETUP_VERSION,
            .client_sni = g_coap_cfg.host,
            .psk_info = {
                .identity = { .s = (uint8_t *)g_coap_cfg.device_token,
                            .length = strlen(g_coap_cfg.device_token) },
                .key = { .s = (uint8_t *)g_coap_cfg.device_token,
                       .length = strlen(g_coap_cfg.device_token) },
            },
        };
        session = coap_new_client_session_psk2(ctx, NULL, &dst, COAP_PROTO_DTLS, &psk);
    } else {
        session = coap_new_client_session(ctx, NULL, &dst, COAP_PROTO_UDP);
    }
    
    if (!session) {
        ESP_LOGE(TAG, "Failed to create CoAP polling session");
        coap_free_context(ctx);
        return ESP_FAIL;
    }
    
    /* Build CoAP GET request with Observe=0 (subscribe) */
    // ThingsBoard requires the Observe option to register for RPC notifications.
    // A plain GET without Observe returns 4.00 Bad Request.
    coap_pdu_t *pdu = coap_new_pdu(COAP_MESSAGE_CON, COAP_REQUEST_CODE_GET, session);
    if (!pdu) {
        ESP_LOGE(TAG, "Failed to allocate CoAP GET PDU");
        coap_session_release(session);
        coap_free_context(ctx);
        return ESP_FAIL;
    }

    /* Observe option: value 0 = subscribe (register as observer) */
    unsigned char observe_val[1] = { 0 };
    coap_add_option(pdu, COAP_OPTION_OBSERVE, sizeof(observe_val), observe_val);

    /* Add Uri-Path options */
    {
        char path_copy[256];
        strncpy(path_copy, resource_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';
        char *segment = strtok(path_copy, "/");
        while (segment) {
            coap_add_option(pdu, COAP_OPTION_URI_PATH,
                            strlen(segment), (const uint8_t *)segment);
            segment = strtok(NULL, "/");
        }
    }

    /* Clear any leftover payload from previous cycle */
    s_rpc_payload_ready = false;

    /* Send GET+Observe request */
    coap_mid_t mid = coap_send(session, pdu);
    if (mid == COAP_INVALID_MID) {
        ESP_LOGE(TAG, "CoAP GET send failed");
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
        coap_session_release(session);
        coap_free_context(ctx);
        return ESP_FAIL;
    }
    
    /* Wait for response with FIXED 2s timeout — separate from poll interval.
       Poll interval controls repeat rate; this only waits for the ACK. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    
    while (xTaskGetTickCount() < deadline) {
        int io_ms = coap_io_process(ctx, 100);
        if (io_ms < 0) break;
    }
    
    /* If a payload arrived (Observe notification with RPC command), process it */
    if (s_rpc_payload_ready) {
        s_rpc_payload_ready = false;
        ESP_LOGI(TAG, "CoAP RPC notification payload: %s", s_rpc_payload_buf);

        /* ThingsBoard sends: {"method":"...","params":"<hex_cmd>","id":<N>} */
        char *id_pos     = strstr(s_rpc_payload_buf, "\"id\":");
        char *params_pos = strstr(s_rpc_payload_buf, "\"params\":");
        
        int rpc_id = -1;
        if (id_pos) rpc_id = atoi(id_pos + 5);
        
        /* Skip if this is a duplicate of the last RPC we processed */
        if (rpc_id >= 0 && rpc_id == s_last_rpc_id) {
            ESP_LOGW(TAG, "CoAP RPC ID %d already processed, skipping duplicate", rpc_id);
            memset(s_rpc_payload_buf, 0, sizeof(s_rpc_payload_buf));
            coap_session_release(session);
            coap_free_context(ctx);
            return ESP_OK;
        }
        
        if (id_pos) s_last_rpc_id = rpc_id;
        if (params_pos) {
            params_pos += 9; /* skip "params": */
            /* Skip leading whitespace and quote */
            while (*params_pos == ' ' || *params_pos == '\"') params_pos++;
            /* Find end quote */
            char *end = strchr(params_pos, '\"');
            if (end) {
                size_t param_len = end - params_pos;
                ESP_LOGI(TAG, "CoAP RPC params (len=%zu): %.*s", param_len, (int)param_len, params_pos);

                /* Convert hex pairs back to binary command string */
                char cmd_buf[256] = {0};
                size_t cmd_len = 0;
                for (size_t i = 0; i + 1 < param_len && cmd_len < sizeof(cmd_buf) - 1; i += 2) {
                    char hex[3] = { params_pos[i], params_pos[i+1], 0 };
                    cmd_buf[cmd_len++] = (char)strtol(hex, NULL, 16);
                }
                cmd_buf[cmd_len] = '\0';
                ESP_LOGI(TAG, "CoAP RPC decoded command: %s", cmd_buf);

                /* Skip leading 'CF' prefix if present */
                const char *cmd_data = (cmd_buf[0] == 'C' && cmd_buf[1] == 'F')
                                       ? cmd_buf + 2 : cmd_buf;
                int cmd_data_len = (int)(cmd_len - (cmd_data - cmd_buf));

                config_type_t type = config_parse_type(cmd_data, cmd_data_len);
                if (type != CONFIG_TYPE_UNKNOWN && g_config_handler_queue) {
                    config_command_t *cmd = malloc(sizeof(config_command_t));
                    if (cmd) {
                        memset(cmd, 0, sizeof(*cmd));
                        cmd->type = type;
                        cmd->data_len = cmd_data_len;
                        cmd->source = CMD_SOURCE_COAP;
                        memcpy(cmd->raw_data, cmd_data, cmd_data_len);
                        cmd->raw_data[cmd_data_len] = '\0';
                        if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
                            ESP_LOGW(TAG, "Config queue full, dropping CoAP RPC command");
                            free(cmd);
                        }
                    }
                }
            }
        }
    }

    ESP_LOGI(TAG, "CoAP RPC polling completed");

    coap_session_release(session);
    coap_free_context(ctx);
    return ESP_OK;
}

/**
 * @brief CoAP RPC polling task
 */
static void coap_polling_task(void *arg) {
    ESP_LOGI(TAG, "CoAP RPC polling task started, interval=%dms", COAP_RPC_POLL_INTERVAL_DEFAULT_MS);
    
    // Wait for first poll before starting
    vTaskDelay(pdMS_TO_TICKS(COAP_RPC_POLL_INTERVAL_DEFAULT_MS));
    
    while (s_polling_running) {
        coap_poll_rpc();
        // Use config value if set, otherwise use default (200ms)
        uint32_t interval_ms = g_coap_cfg.rpc_poll_interval_ms > 0
                               ? g_coap_cfg.rpc_poll_interval_ms
                               : COAP_RPC_POLL_INTERVAL_DEFAULT_MS;
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
    
    ESP_LOGI(TAG, "CoAP RPC polling task exiting");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void coap_publish_task(void *arg) {
    coap_publish_data_t item;
    uint8_t *data_buffer = (uint8_t *)malloc(COAP_PUBLISH_DATA_MAX_LEN);
    char *json_payload = (char *)malloc(COAP_JSON_PAYLOAD_CAP);
    
    if (!data_buffer || !json_payload) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "CoAP publish task started");

    while (s_task_running) {
        if (xQueueReceive(g_coap_publish_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (item.length == 0) continue;
            
            // Keep full queued payload (up to COAP_PUBLISH_DATA_MAX_LEN) for throughput tests.
            size_t copy_len = (item.length < COAP_PUBLISH_DATA_MAX_LEN)
                                  ? item.length
                                  : COAP_PUBLISH_DATA_MAX_LEN;
            memcpy(data_buffer, item.data, copy_len);
            
            /* Format as JSON for ThingsBoard: {"data":"HEX_STRING"} */
            size_t json_cap = COAP_JSON_PAYLOAD_CAP;
            int json_len = snprintf(json_payload, json_cap, "{\"data\":\"");
            for (size_t i = 0; i < copy_len && json_len < (int)json_cap - 3; i++) {
                json_len += snprintf(&json_payload[json_len], json_cap - json_len, "%02X", data_buffer[i]);
            }
            json_len += snprintf(&json_payload[json_len], json_cap - json_len, "\"}");

            if (json_len >= (int)json_cap) {
                ESP_LOGW(TAG, "CoAP JSON truncated: raw=%zu json_len=%d cap=%zu", copy_len, json_len, json_cap);
                json_len = (int)json_cap - 1;
                json_payload[json_len] = '\0';
            }

            ESP_LOGI(TAG, "Publishing %zu bytes as JSON (%d chars)", copy_len, json_len);

            /* Build base telemetry path */
            char telem_path[256];
            build_resource_path(g_coap_cfg.resource_path, g_coap_cfg.device_token,
                                telem_path, sizeof(telem_path));

            /* Always POST to /telemetry so ThingsBoard dashboard shows latest data */
            esp_err_t rc = coap_post_to_resource(telem_path, (const uint8_t *)json_payload, json_len);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "CoAP telemetry publish failed");
            }

            /* Also POST to /rpc/{id} so the widget's RPC promise can resolve */
            if (s_last_rpc_id >= 0) {
                char rpc_path[256];
                strncpy(rpc_path, telem_path, sizeof(rpc_path) - 1);
                rpc_path[sizeof(rpc_path) - 1] = '\0';
                char *telem = strstr(rpc_path, "/telemetry");
                if (telem) {
                    snprintf(telem, sizeof(rpc_path) - (size_t)(telem - rpc_path),
                             "/rpc/%d", s_last_rpc_id);
                }
                ESP_LOGI(TAG, "Also routing to RPC response: %s (id=%d)", rpc_path, s_last_rpc_id);
                coap_post_to_resource(rpc_path, (const uint8_t *)json_payload, json_len);
                /* NOTE: s_last_rpc_id is NOT reset here — kept for poll dedup */
            }

            if (COAP_POST_PUBLISH_DELAY_MS > 0) {
                vTaskDelay(pdMS_TO_TICKS(COAP_POST_PUBLISH_DELAY_MS));
            } else {
                taskYIELD();
            }
        }
    }

    ESP_LOGI(TAG, "CoAP publish task exiting");
    
cleanup:
    if (data_buffer) free(data_buffer);
    if (json_payload) free(json_payload);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void coap_handler_task_start(void) {
    if (s_task_running) {
        ESP_LOGW(TAG, "CoAP handler already running");
        return;
    }

    if (!g_coap_publish_queue) {
        size_t q_bytes = COAP_QUEUE_SIZE * sizeof(coap_publish_data_t);
        s_coap_publish_qstorage = (uint8_t *)heap_caps_malloc(q_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_coap_publish_qcb = (StaticQueue_t *)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        if (s_coap_publish_qstorage && s_coap_publish_qcb) {
            g_coap_publish_queue = xQueueCreateStatic(
                COAP_QUEUE_SIZE,
                sizeof(coap_publish_data_t),
                s_coap_publish_qstorage,
                s_coap_publish_qcb);
        }

        if (!g_coap_publish_queue) {
            if (s_coap_publish_qstorage) {
                heap_caps_free(s_coap_publish_qstorage);
                s_coap_publish_qstorage = NULL;
            }
            if (s_coap_publish_qcb) {
                heap_caps_free(s_coap_publish_qcb);
                s_coap_publish_qcb = NULL;
            }
            ESP_LOGE(TAG, "Failed to create CoAP publish queue");
            return;
        }
        ESP_LOGI(TAG, "CoAP publish queue created: depth=%d", COAP_QUEUE_SIZE);
    }

    s_coap_server_connected = false;
    s_task_running = true;
    {
        StackType_t *pub_stack = (StackType_t *)heap_caps_malloc(6144, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *pub_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (pub_stack && pub_tcb) {
            s_task_handle = xTaskCreateStatic(coap_publish_task, "coap_publish", 6144, NULL, 5, pub_stack, pub_tcb);
            ESP_LOGI(TAG, "CoAP handler task created in PSRAM");
        } else {
            ESP_LOGE(TAG, "Failed to allocate coap_publish task in PSRAM");
        }
    }
    
    // Start polling task for RPC commands
    s_polling_running = true;
    {
        StackType_t *poll_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *poll_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (poll_stack && poll_tcb) {
            s_polling_task_handle = xTaskCreateStatic(coap_polling_task, "coap_poll_rpc", 4096, NULL, 4, poll_stack, poll_tcb);
            ESP_LOGI(TAG, "CoAP RPC polling task created in PSRAM");
        } else {
            ESP_LOGE(TAG, "Failed to allocate coap_poll_rpc task in PSRAM");
        }
    }
}

void coap_handler_task_stop(void) {
    s_task_running = false;
    s_task_handle = NULL;
    s_polling_running = false;
    s_polling_task_handle = NULL;
    s_coap_server_connected = false;
    /* Delete the queue so its internal-RAM storage is returned to the heap.
     * Without this, the next handler that calls xQueueCreate() may fail even
     * though total RAM is plentiful, because the old queue still holds the
     * internal-RAM allocation.                                              */
    if (g_coap_publish_queue) {
        vQueueDelete(g_coap_publish_queue);
        g_coap_publish_queue = NULL;
        if (s_coap_publish_qstorage) {
            heap_caps_free(s_coap_publish_qstorage);
            s_coap_publish_qstorage = NULL;
        }
        if (s_coap_publish_qcb) {
            heap_caps_free(s_coap_publish_qcb);
            s_coap_publish_qcb = NULL;
        }
    }
    ESP_LOGI(TAG, "CoAP handler tasks stopped");
}

bool coap_handler_is_connected(void) {
    return s_coap_server_connected;
}

bool coap_enqueue_telemetry(const uint8_t *data, size_t data_len) {
    if (!data || data_len == 0 || !g_coap_publish_queue) {
        return false;
    }

    if (data_len > COAP_PUBLISH_DATA_MAX_LEN) {
        ESP_LOGW(TAG, "Payload too large (%zu > %d), truncating",
                 data_len, COAP_PUBLISH_DATA_MAX_LEN);
        data_len = COAP_PUBLISH_DATA_MAX_LEN;
    }

    coap_publish_data_t item;
    memcpy(item.data, data, data_len);
    item.length = data_len;

    if (xQueueSend(g_coap_publish_queue, &item, pdMS_TO_TICKS(COAP_ENQUEUE_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "CoAP publish queue full payload dropped");
        return false;
    }

    return true;
}

void coap_handler_update_config(const coap_config_data_t *cfg) {
    if (!cfg) return;
    memcpy(&g_coap_cfg, cfg, sizeof(coap_config_data_t));
    ESP_LOGI(TAG, "CoAP config updated Host: %s", g_coap_cfg.host);
}
