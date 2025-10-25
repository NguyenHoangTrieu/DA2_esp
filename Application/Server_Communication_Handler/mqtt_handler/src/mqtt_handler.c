/*
 * MQTT handler module for ESP32-S3 board.
 * Manages MQTT connection, publishing telemetry data, and receiving commands.
 * Comments are in English for clarity.
 */
#include "mqtt_handler.h"
#include "config_handler.h"

#define BROKER_URI "mqtt://demo.thingsboard.io:1883"
#define DEVICE_TOKEN "ZCOjw6KKw5j2EqYV2co6"
#define PUBLISH_TOPIC "v1/devices/me/telemetry"
#define SUBSCRIBE_TOPIC "v1/devices/me/rpc/request/+"
#define ATRIBUTE_TOPIC "v1/devices/me/attributes"
#define PUBLISH_INTERVAL 1000 // ms

static const char *TAG = "mqtt_handler";
static esp_mqtt_client_handle_t m_client = NULL;
static TaskHandle_t m_pub_task = NULL;
static volatile uint8_t m_mqtt_connected = false;
static volatile uint8_t s_mqtt_payload_updated = false;
static char s_mqtt_payload[1024] = {0};
QueueHandle_t g_server_cmd_queue = NULL;
static bool mqtt_task_close = false;

// Current MQTT config (can be updated from queue)
static char s_broker_uri[128] = BROKER_URI;
static char s_device_token[65] = DEVICE_TOKEN;

void mqtt_receive_enqueue(const char *data, size_t len) {
  ESP_LOGI(TAG, "Received data to enqueue: %.*s", (int)len, data);
  if (!g_server_cmd_queue)
    return;
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
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(event->client, ATRIBUTE_TOPIC, 1);
    esp_mqtt_client_subscribe(event->client, SUBSCRIBE_TOPIC, 1);
    esp_mqtt_client_publish(event->client, ATRIBUTE_TOPIC,
                            "{\"chip_type\":\"esp32p4\", \"fw\":\"1.0.0\"}", 0,
                            1, 0);
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
 * FreeRTOS task to publish a message every 1 second when MQTT is connected.
 */
static void mqtt_publish_task(void *arg) {
  while (!mqtt_task_close) {
    // Only publish if MQTT client is connected
    if (m_client && m_mqtt_connected && s_mqtt_payload_updated) {
      esp_mqtt_client_publish(m_client, PUBLISH_TOPIC, s_mqtt_payload, 0, 1, 0);
      ESP_LOGI(TAG, "Published: %s", s_mqtt_payload);
      s_mqtt_payload_updated = false; // Reset flag after publishing
    }
    vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL));
  }
  ESP_LOGI(TAG, "MQTT publish task exiting.");
  vTaskDelete(NULL);
}

/*
 * Re-initialize MQTT client with new config
 */
static void mqtt_reinit(void) {
  // Stop existing client
  if (m_client) {
    ESP_LOGI(TAG, "Stopping existing MQTT client");
    esp_mqtt_client_stop(m_client);
    esp_mqtt_client_destroy(m_client);
    m_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Create new client with updated config
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker = {
          .address = {
              .uri = s_broker_uri,
          },
      },
      .credentials = {
          .username = s_device_token,
      }};

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  if (m_client) {
    esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    esp_mqtt_client_start(m_client);
    ESP_LOGI(TAG, "MQTT client reinitialized with URI: %s, Token: %s",
             s_broker_uri, s_device_token);
  } else {
    ESP_LOGE(TAG, "Failed to reinitialize MQTT client");
  }
}

/*
 * FreeRTOS task to monitor config queue for MQTT config updates
 */
static void mqtt_config_task(void *arg) {
  mqtt_config_data_t mqtt_cfg;

  ESP_LOGI(TAG, "MQTT config task started, listening for config from queue");

  while (!mqtt_task_close) {
    // Check for MQTT config from queue
    if (g_mqtt_config_queue != NULL) {
      if (xQueueReceive(g_mqtt_config_queue, &mqtt_cfg, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        ESP_LOGI(TAG, "Received MQTT config from queue");
        ESP_LOGI(TAG, "Broker: %s, Port: %d, Token: %s", mqtt_cfg.broker_uri,
                 mqtt_cfg.port, mqtt_cfg.device_token);

        // Update configuration
        strncpy(s_broker_uri, mqtt_cfg.broker_uri, sizeof(s_broker_uri) - 1);
        s_broker_uri[sizeof(s_broker_uri) - 1] = '\0';
        strncpy(s_device_token, mqtt_cfg.device_token,
                sizeof(s_device_token) - 1);
        s_device_token[sizeof(s_device_token) - 1] = '\0';

        // Reinitialize MQTT client with new config
        mqtt_reinit();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "MQTT config task exiting.");
  vTaskDelete(NULL);
}

/*
 * Initialize MQTT client and start publishing task.
 * Call this once at startup.
 */
void mqtt_handler_task_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker = {
          .address = {
              .uri = s_broker_uri,
          },
      },
      .credentials = {
          .username = s_device_token,
      }};

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 NULL);
  esp_mqtt_client_start(m_client);

  if (!g_server_cmd_queue)
    g_server_cmd_queue = xQueueCreate(8, 128); // 8 slots, 128 bytes payload

  // Start publish task
  xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5,
              &m_pub_task);

  // Start config monitoring task
  xTaskCreate(mqtt_config_task, "mqtt_config_task", 3072, NULL, 5, NULL);

  ESP_LOGI(TAG, "MQTT handler tasks started");
}

/*
 * Stop MQTT client and delete publishing task.
 */
void mqtt_handler_task_stop(void) {
  mqtt_task_close = true;
  if (m_client) {
    esp_mqtt_client_stop(m_client);
    esp_mqtt_client_destroy(m_client);
    m_client = NULL;
  }
  ESP_LOGI(TAG, "MQTT handler tasks stopped");
}

/* MQTT build telemetry data from source to payload buffer and clear the source
 */
void mqtt_build_telemetry_payload(char *source, size_t len) {
  memset(s_mqtt_payload, 0, sizeof(s_mqtt_payload));
  memcpy(s_mqtt_payload, source, len);
  s_mqtt_payload_updated = true;
  memset(source, 0, len); // Clear source after copying
}
