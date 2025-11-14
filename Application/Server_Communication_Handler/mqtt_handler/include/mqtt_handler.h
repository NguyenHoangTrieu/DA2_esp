/*
 * MQTT handler module header for ESP32-S3 board.
 */
#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <stddef.h>

// NEW: Data type enum for publish task
typedef enum {
    MQTT_DATA_TYPE_TELEMETRY,  // Small telemetry data
    MQTT_DATA_TYPE_LARGE       // Large binary data (32KB)
} mqtt_data_type_t;

// NEW: Unified publish data structure
typedef struct {
    mqtt_data_type_t type;
    uint8_t *data;
    size_t length;
} mqtt_publish_data_t;

// Start the MQTT handler, launch FreeRTOS publishing task in suspended state.
void mqtt_handler_task_start(void);

// Stop the MQTT handler, delete the publishing task.
void mqtt_handler_task_stop(void);

// Build telemetry data from source buffer to internal payload buffer and clear source.
void mqtt_build_telemetry_payload(char *source, size_t len);

// NEW: Enqueue large data for publishing
void mqtt_enqueue_large_data(const uint8_t *data, size_t data_len);

void mqtt_receive_enqueue(const char *data, size_t len);

extern QueueHandle_t g_server_cmd_queue;
extern QueueHandle_t g_mqtt_publish_queue;  // NEW: Unified publish queue

#endif // MQTT_HANDLER_H
