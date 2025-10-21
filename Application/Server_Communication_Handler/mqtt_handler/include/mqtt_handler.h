/*
 * MQTT handle module for ESP32-P4 - Demo to send telemetry to ThingsBoard
 * The send task resumes only after WiFi is connected and suspends when WiFi is lost.
 * All comments are in English.
 */

#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"
// Start the MQTT handle, launch FreeRTOS publishing task in suspended state.
void mqtt_handle_start(void);

// Called from WiFi connect event: resumes MQTT publishing task.
void mqtt_handle_resume(void);

// Called from WiFi disconnect event: suspends MQTT publishing task.
void mqtt_handle_suspend(void);

// Build telemetry data from source buffer to internal payload buffer and clear source.
void mqtt_build_telemetry_payload(char *source, size_t len);

void mqtt_receive_enqueue(const char *data, size_t len);

extern QueueHandle_t g_server_cmd_queue;

#endif // MQTT_HANDLE_H
