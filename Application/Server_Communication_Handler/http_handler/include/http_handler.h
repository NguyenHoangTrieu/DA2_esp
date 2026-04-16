/**
 * @file http_handler.h
 * @brief HTTP/HTTPS server handler for telemetry publishing
 *
 * Sends JSON telemetry payloads via HTTP POST to a configurable endpoint.
 * Supports Bearer-token auth, TLS, and configurable timeout.
 * Uses a FreeRTOS queue so callers never block.
 */

#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "config_handler.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Publish queue item
// ---------------------------------------------------------------------------
#define HTTP_PUBLISH_DATA_MAX_LEN 512

typedef struct {
    uint8_t data[HTTP_PUBLISH_DATA_MAX_LEN];
    size_t  length;
} http_publish_data_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/** Start the HTTP handler task and initialize the publish queue */
void http_handler_task_start(void);

/** Stop the HTTP handler task */
void http_handler_task_stop(void);

/**
 * @brief Enqueue a telemetry payload for HTTP publishing (non-blocking).
 * @param data Pointer to payload bytes
 * @param data_len Payload length in bytes
 * @return true if enqueued successfully, false if queue full or not started
 */
bool http_enqueue_telemetry(const uint8_t *data, size_t data_len);

/**
 * @brief Update running HTTP configuration (takes effect on next publish).
 * @param cfg New configuration to apply
 */
void http_handler_update_config(const http_config_data_t *cfg);

// Publish queue handle (extern for routing)
extern QueueHandle_t g_http_publish_queue;

#endif // HTTP_HANDLER_H
