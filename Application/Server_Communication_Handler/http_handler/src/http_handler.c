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
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "HTTP_HANDLER";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/** Live configuration — written by config_handler via http_handler_update_config() */
extern http_config_data_t g_http_cfg;

QueueHandle_t g_http_publish_queue = NULL;

static TaskHandle_t  s_task_handle   = NULL;
static bool          s_task_running  = false;

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
 * Perform a single HTTP POST of `payload` to the currently configured endpoint.
 * Returns ESP_OK on 2xx response, ESP_FAIL otherwise.
 */
static esp_err_t http_post_payload(const uint8_t *payload, size_t len) {
    char url[512];
    build_url(g_http_cfg.server_url, g_http_cfg.auth_token, url, sizeof(url));

    esp_http_client_config_t config = {
        .url             = url,
        .method          = HTTP_METHOD_POST,
        .timeout_ms      = (int)g_http_cfg.timeout_ms,
        .skip_cert_common_name_check = !g_http_cfg.verify_server,
        .transport_type  = g_http_cfg.use_tls
                               ? HTTP_TRANSPORT_OVER_SSL
                               : HTTP_TRANSPORT_OVER_TCP,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", HTTP_CONTENT_TYPE);

    // If URL has no {token}, use Bearer header instead
    if (g_http_cfg.auth_token[0] != '\0' && strstr(g_http_cfg.server_url, "{token}") == NULL) {
        char auth_hdr[150];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", g_http_cfg.auth_token);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }

    esp_http_client_set_post_field(client, (const char *)payload, (int)len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "HTTP POST OK – status %d, %zu bytes sent", status, len);
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

static void http_publish_task(void *arg) {
    http_publish_data_t item;
    ESP_LOGI(TAG, "HTTP publish task started");

    while (s_task_running) {
        if (xQueueReceive(g_http_publish_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (item.length == 0) continue;
            ESP_LOGI(TAG, "Dequeued %zu bytes for HTTP publish", item.length);
            esp_err_t rc = http_post_payload(item.data, item.length);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "HTTP publish failed – payload dropped");
            }
        }
    }

    ESP_LOGI(TAG, "HTTP publish task exiting");
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
    xTaskCreate(http_publish_task, "http_publish", 4096, NULL, 5, &s_task_handle);
    ESP_LOGI(TAG, "HTTP handler task created");
}

void http_handler_task_stop(void) {
    s_task_running = false;
    s_task_handle  = NULL;
    ESP_LOGI(TAG, "HTTP handler task stopped");
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
        ESP_LOGE(TAG, "HTTP publish queue full – payload dropped");
        return false;
    }

    return true;
}

void http_handler_update_config(const http_config_data_t *cfg) {
    if (!cfg) return;
    // g_http_cfg is the live global; copy in new settings.
    // Note: ongoing requests in the task will use the new config on next iteration.
    memcpy(&g_http_cfg, cfg, sizeof(http_config_data_t));
    ESP_LOGI(TAG, "HTTP config updated – URL: %s", g_http_cfg.server_url);
}
