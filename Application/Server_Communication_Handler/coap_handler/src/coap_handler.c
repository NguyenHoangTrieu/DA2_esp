/**
 * @file coap_handler.c
 * @brief CoAP client telemetry publisher using ESP-IDF libcoap (coap3).
 */

#include "coap_handler.h"
#include "config_handler.h"
#include "esp_log.h"
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

QueueHandle_t g_coap_publish_queue = NULL;

static TaskHandle_t  s_task_handle  = NULL;
static bool          s_task_running = false;

#define COAP_QUEUE_SIZE 8
#define COAP_ACK_TIMEOUT_DEFAULT_MS 2000
#define COAP_WAIT_TICKS pdMS_TO_TICKS(5000)

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
    if (COAP_RESPONSE_CLASS(code) != 2)
        ESP_LOGE(TAG, "CoAP error %d.%02d MID=%d",
                 COAP_RESPONSE_CLASS(code), code & 0x1F, mid);
    else
        ESP_LOGI(TAG, "CoAP ACK %d.%02d MID=%d",
                 COAP_RESPONSE_CLASS(code), code & 0x1F, mid);
    return COAP_RESPONSE_OK;
}

/**
* @brief CoAP NACK handler
*/
static void coap_nack_handler(coap_session_t *s, const coap_pdu_t *sent,
                               coap_nack_reason_t reason, coap_mid_t mid) {
    ESP_LOGE(TAG, "CoAP NACK reason=%d MID=%d", reason, mid);
}

/**
 * Send a single CoAP PUT (Confirmable) with the given payload.
 */
static esp_err_t coap_send_payload(const uint8_t *payload, size_t len) {
    char resource_path[256];
    build_resource_path(g_coap_cfg.resource_path, g_coap_cfg.device_token,
                        resource_path, sizeof(resource_path));

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

    /* Send */
    coap_mid_t mid = coap_send(session, pdu);
    if (mid == COAP_INVALID_MID) {
        ESP_LOGE(TAG, "CoAP send failed");
        coap_session_release(session);
        coap_free_context(ctx);
        return ESP_FAIL;
    }

    /* Pump the I/O loop until ACK received or timeout */
    uint32_t ack_timeout_ms = g_coap_cfg.ack_timeout_ms > 0
                                  ? g_coap_cfg.ack_timeout_ms
                                  : COAP_ACK_TIMEOUT_DEFAULT_MS;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ack_timeout_ms * 2);

    while (xTaskGetTickCount() < deadline) {
        int io_ms = coap_io_process(ctx, 100 /* ms */);
        if (io_ms < 0) break;
    }

    ESP_LOGI(TAG, "CoAP POST sent MID %d, path: %s, %zu bytes", mid, resource_path, len);

    coap_session_release(session);
    coap_free_context(ctx);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void coap_publish_task(void *arg) {
    coap_publish_data_t item;
    ESP_LOGI(TAG, "CoAP publish task started");

    while (s_task_running) {
        if (xQueueReceive(g_coap_publish_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (item.length == 0) continue;
            ESP_LOGI(TAG, "Dequeued %zu bytes for CoAP publish", item.length);
            esp_err_t rc = coap_send_payload(item.data, item.length);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "CoAP publish failed – payload dropped");
            }
        }
    }

    ESP_LOGI(TAG, "CoAP publish task exiting");
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
        g_coap_publish_queue = xQueueCreate(COAP_QUEUE_SIZE, sizeof(coap_publish_data_t));
        if (!g_coap_publish_queue) {
            ESP_LOGE(TAG, "Failed to create CoAP publish queue");
            return;
        }
    }

    s_task_running = true;
    xTaskCreate(coap_publish_task, "coap_publish", 6144, NULL, 5, &s_task_handle);
    ESP_LOGI(TAG, "CoAP handler task created");
}

void coap_handler_task_stop(void) {
    s_task_running = false;
    s_task_handle  = NULL;
    ESP_LOGI(TAG, "CoAP handler task stopped");
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

    if (xQueueSend(g_coap_publish_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
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
