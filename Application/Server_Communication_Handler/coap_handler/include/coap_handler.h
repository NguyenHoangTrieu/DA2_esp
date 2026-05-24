/**
 * @file coap_handler.h
 * @brief CoAP client handler for telemetry publishing
 *
 * Sends JSON telemetry payloads via CoAP PUT/POST to a configurable server.
 * Supports plain UDP (CoAP) and DTLS (CoAPS).
 * Token substitution (`{token}` in resource path) is performed automatically.
 */

#ifndef COAP_HANDLER_H
#define COAP_HANDLER_H

#include "config_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Publish queue item
// ---------------------------------------------------------------------------
/* Inline queue item payload sized to match MQTT/HTTP so large uplinks can
 * be forwarded consistently across all server communication types. */
#define COAP_PUBLISH_DATA_MAX_LEN 2048

typedef struct {
    uint8_t data[COAP_PUBLISH_DATA_MAX_LEN];
    size_t  length;
    /* [E2E_TOTAL] fields. Zero when bench is off / handler didn't measure. */
    int64_t lan_rx_us;         // absolute LAN esp_timer_get_time() from DT frame
    int64_t wan_rx_us;         // when WAN MCU first parsed the SPI frame
    char    handler_type[4];
} coap_publish_data_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/** Start the CoAP handler task and initialize the publish queue */
void coap_handler_task_start(void);

/** Stop the CoAP handler task */
void coap_handler_task_stop(void);

/** Return true when the last CoAP exchange reached the configured server. */
bool coap_handler_is_connected(void);

/**
 * @brief Enqueue a telemetry payload for CoAP publishing (non-blocking).
 * @param data Pointer to payload bytes
 * @param data_len Payload length in bytes
 * @return true if enqueued successfully, false if queue full or not started
 */
bool coap_enqueue_telemetry(const uint8_t *data, size_t data_len);

/* E2E-tagged variant for [E2E_TOTAL] logging. */
bool coap_enqueue_telemetry_e2e(const uint8_t *data, size_t data_len,
                                int64_t lan_rx_us, int64_t wan_rx_us,
                                const char *handler_type);

/**
 * @brief Update running CoAP configuration (takes effect on next publish).
 * @param cfg New configuration to apply
 */
void coap_handler_update_config(const coap_config_data_t *cfg);

// Publish queue handle (extern for routing)
extern QueueHandle_t g_coap_publish_queue;

#endif // COAP_HANDLER_H
