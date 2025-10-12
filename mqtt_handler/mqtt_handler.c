#include "mqtt_handler.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BROKER_URI        "mqtt://demo.thingsboard.io:1883"
#define DEVICE_TOKEN      "ZCOjw6KKw5j2EqYV2co6"
#define PUBLISH_TOPIC     "v1/devices/me/telemetry"
#define SUBSCRIBE_TOPIC   "v1/devices/me/rpc/request/+"
#define ATRIBUTE_TOPIC    "v1/devices/me/attributes"
#define PUBLISH_INTERVAL  1000  // ms

static const char *TAG = "mqtt_handler";
static esp_mqtt_client_handle_t m_client = NULL;
static TaskHandle_t m_pub_task = NULL;
static volatile uint8_t m_mqtt_connected = false;
/*
 * MQTT event handler function.
 * Updates connection state and logs events.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(client, "v1/devices/me/attributes", 1);
            esp_mqtt_client_subscribe(client, "v1/devices/me/rpc/request/+", 1);
            m_mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            m_mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            m_mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
            break;
        default:
            break;
    }
}

/*
 * FreeRTOS task to publish a message every 10 seconds when MQTT is connected.
 * Suspends automatically if MQTT is disconnected.
 */
static void mqtt_publish_task(void *arg)
{
    int counter = 0;
    while (1) {
        // Only publish if MQTT client is connected
        if (m_client && m_mqtt_connected) {
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"count\": %d}", counter++);
            esp_mqtt_client_publish(m_client, PUBLISH_TOPIC, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published: %s", payload);
        }
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL));
    }
}

/*
 * Initialize MQTT client and start publishing task (default is suspend).
 * Call this once at startup.
 */
void mqtt_handle_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = BROKER_URI,
            },
        },
        .credentials = {
            .username = DEVICE_TOKEN,
        }
    };
    m_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(m_client);
    // Start publish task and immediately suspend it
    xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, &m_pub_task);
    vTaskSuspend(m_pub_task);
}

/*
 * Resume the MQTT publish task after WiFi/MQTT connection is established.
 */
void mqtt_handle_resume(void)
{
    if (m_pub_task)
        vTaskResume(m_pub_task);
}

/*
 * Suspend the MQTT publish task when WiFi/MQTT disconnects.
 */
void mqtt_handle_suspend(void)
{
    if (m_pub_task)
        vTaskSuspend(m_pub_task);
}
