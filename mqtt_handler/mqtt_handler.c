/*
 * MQTT handle module implementation for ESP32-P4 using ESP-IDF's esp-mqtt.
 * Task is suspended until WiFi connects, resumes to publish data, and suspends again if WiFi disconnects.
 * Data is published to demo.thingsboard.io as required by the task.
 */

#include "mqtt_handler.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MQTT_SERVER_URI "mqtt://demo.thingsboard.io:1883"
#define THINGSBOARD_TOKEN "YOUR_THINGSBOARD_DEVICE_TOKEN" // <-- Replace with your token
#define MQTT_PUB_TOPIC "v1/devices/me/telemetry"
#define PUBLISH_PERIOD_MS 10000  // Publish every 10 seconds

static const char *TAG = "MQTT_HANDLE";
static esp_mqtt_client_handle_t client = NULL;
static TaskHandle_t mqtt_task_handle = NULL;

/* MQTT event handler (simple logging & connection state) */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        break;
    default:
        break;
    }
}

/* This is the FreeRTOS MQTT task. It is started suspended.
 * When resumed (i.e. after WiFi connects and IP obtained), it will publish periodically.
 * If suspended, it will stop sending until resumed again.
 */
static void mqtt_send_task(void *pvParameters)
{
    int count = 0;

    while (1) {
        // Wait if suspended (FreeRTOS: the task will not consume CPU)
        // Note: vTaskSuspend(NULL) suspends itself (this task)
        // Resume from another task or event via vTaskResume
        if (client == NULL || !esp_mqtt_client_is_connected(client)) {
            ESP_LOGI(TAG, "MQTT not ready, suspending MQTT task");
            vTaskSuspend(NULL); // Suspend myself
        }

        // If client is connected, publish
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"count\":%d}", count++);
        esp_mqtt_client_publish(client, MQTT_PUB_TOPIC, payload, 0, 1, 0);
        ESP_LOGI(TAG, "MQTT Published: %s", payload);
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
    }
}

/* Start the MQTT client and send task */
void mqtt_handle_start(void)
{
    // Configure and start MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = MQTT_SERVER_URI,
        .username = THINGSBOARD_TOKEN,
        // .password = "", // Not needed for ThingsBoard demo
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    // Launch publishing task, immediately suspended
    xTaskCreate(mqtt_send_task, "mqtt_send_task", 4096, NULL, 5, &mqtt_task_handle);
    vTaskSuspend(mqtt_task_handle);
}

/* Call from WiFi event handler (e.g. when IP acquired) */
void mqtt_handle_resume(void)
{
    if (mqtt_task_handle != NULL) {
        vTaskResume(mqtt_task_handle);
        ESP_LOGI(TAG, "MQTT task resumed");
    }
}

/* Call from WiFi event handler on disconnect */
void mqtt_handle_suspend(void)
{
    if (mqtt_task_handle != NULL) {
        vTaskSuspend(mqtt_task_handle);
        ESP_LOGI(TAG, "MQTT task suspended");
    }
}
