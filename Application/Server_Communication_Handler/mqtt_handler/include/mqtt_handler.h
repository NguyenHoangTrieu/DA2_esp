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
#include <stddef.h>
// Start the MQTT handle, launch FreeRTOS publishing task in suspended state.
void mqtt_handler_task_start(void);

// Stop the MQTT handle, delete the publishing task.
void mqtt_handler_task_stop(void);

// Build telemetry data from source buffer to internal payload buffer and clear
// source.
void mqtt_build_telemetry_payload(char *source, size_t len);

void mqtt_receive_enqueue(const char *data, size_t len);

extern QueueHandle_t g_server_cmd_queue;

#endif // MQTT_HANDLE_H
