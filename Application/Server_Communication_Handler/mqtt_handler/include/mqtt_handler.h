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
#include <string.h>

// Simplified publish data structure (no type differentiation)
typedef struct {
  uint8_t *data;
  size_t length;
} mqtt_publish_data_t;

// Start the MQTT handler
void mqtt_handler_task_start(void);

// Stop the MQTT handler
void mqtt_handler_task_stop(void);

// Unified function to enqueue any data for publishing
void mqtt_enqueue_telemetry(const uint8_t *data, size_t data_len);

// Receive data from MQTT subscription
void mqtt_receive_enqueue(const char *data, size_t len);

extern QueueHandle_t g_server_cmd_queue;
extern QueueHandle_t g_mqtt_publish_queue;

// Global Config variables:
typedef struct {
  char broker_uri[128];
  char device_token[65];
  char subscribe_topic[128];
  char attribute_topic[128];
  char publish_topic[128];
} mqtt_config_context_t;

extern char g_broker_uri[128];
extern char g_device_token[65];
extern char g_subscribe_topic[128];
extern char g_attribute_topic[128];
extern char g_publish_topic[128];

#endif // MQTT_HANDLER_H
