#include "mqtt_handler.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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
static volatile uint8_t s_mqtt_payload_updated = false;
static char s_mqtt_payload[1024] = {0};
QueueHandle_t g_server_cmd_queue = NULL;

void mqtt_receive_enqueue(const char *data, size_t len)
{
    ESP_LOGI(TAG, "Received data to enqueue: %.*s", (int)len, data);
    if (!g_server_cmd_queue) return;
    char buf[128];
    int copy_len = len < 127 ? (int)len : 127;
    memcpy(buf, data, copy_len);
    buf[copy_len] = 0;
    xQueueSend(g_server_cmd_queue, buf, 0);
}

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
            esp_mqtt_client_subscribe(event->client, ATRIBUTE_TOPIC, 1);
            esp_mqtt_client_subscribe(event->client, SUBSCRIBE_TOPIC, 1);
            esp_mqtt_client_publish(event->client, ATRIBUTE_TOPIC, "{\"chip_type\":\"esp32p4\", \"fw\":\"1.0.0\"}", 0, 1, 0);
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
            mqtt_receive_enqueue(event->data, event->data_len);
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

    while (1) {
        // Only publish if MQTT client is connected
        if (m_client && m_mqtt_connected && s_mqtt_payload_updated) {
            esp_mqtt_client_publish(m_client, PUBLISH_TOPIC, s_mqtt_payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published: %s", s_mqtt_payload);
            s_mqtt_payload_updated = false; // Reset flag after publishing
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
    if (!g_server_cmd_queue)
        g_server_cmd_queue = xQueueCreate(8, 128); // 8 slots, 128 bytes payload
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

/* MQTT build telematry data from source to payload buffer and clear the source */
void mqtt_build_telemetry_payload(char *source, size_t len)
{
    memcpy(s_mqtt_payload, source, len);
    s_mqtt_payload_updated = true;
    memset(source, 0, len); // Clear source after copying
}