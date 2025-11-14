/*
 * MQTT handler module for ESP32-S3 board.
 * Manages MQTT connection, publishing telemetry data, and receiving commands.
 * Uses single unified publish task for all data types.
 */

#include "mqtt_handler.h"
#include "config_handler.h"

#define BROKER_URI "mqtt://demo.thingsboard.io:1883"
#define DEVICE_TOKEN "pxmd2c2cpqfio2fksa3m"
#define PUBLISH_TOPIC "v1/devices/me/telemetry"
#define SUBSCRIBE_TOPIC "v1/devices/me/rpc/request/+"
#define ATRIBUTE_TOPIC "v1/devices/me/attributes"
#define PUBLISH_INTERVAL 1000              // ms
#define LARGE_DATA_BUFFER_SIZE (32 * 1024) // 32KB

static const char *TAG = "mqtt_handler";

static esp_mqtt_client_handle_t m_client = NULL;
static TaskHandle_t m_pub_task = NULL;
static volatile uint8_t m_mqtt_connected = false;
static volatile uint8_t s_mqtt_payload_updated = false;
static char s_mqtt_payload[1024] = {0};

QueueHandle_t g_server_cmd_queue = NULL;
QueueHandle_t g_mqtt_publish_queue = NULL; // Unified queue for all publish data

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
                            "{\"chip_type\":\"esp32s3\", \"fw\":\"1.0.0\"}", 0,
                            1, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
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
 * UNIFIED FreeRTOS task to publish both telemetry and large data.
 * Handles two data types:
 * 1. Small telemetry data (from s_mqtt_payload)
 * 2. Large binary data (from queue)
 */
static void mqtt_publish_task(void *arg) {
  mqtt_publish_data_t incoming_data;
  uint8_t *large_data_buffer = NULL;
  TickType_t last_telemetry_time = xTaskGetTickCount();

  // Allocate buffer for large data
  large_data_buffer = heap_caps_malloc(LARGE_DATA_BUFFER_SIZE, MALLOC_CAP_8BIT);
  if (!large_data_buffer) {
    ESP_LOGE(TAG, "Failed to allocate large data buffer");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Unified MQTT publish task started");

  while (!mqtt_task_close) {
    // Priority 1: Check for large data from queue (non-blocking)
    if (g_mqtt_publish_queue &&
        xQueueReceive(g_mqtt_publish_queue, &incoming_data, 0) == pdTRUE) {

      if (incoming_data.type == MQTT_DATA_TYPE_LARGE) {
        // Handle large binary data
        if (incoming_data.data && incoming_data.length > 0) {
          // Wait for MQTT connection
          int wait_count = 0;
          while (!m_mqtt_connected && wait_count < 50) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
          }

          if (m_mqtt_connected) {
            // Copy to local buffer
            size_t copy_len = incoming_data.length < LARGE_DATA_BUFFER_SIZE
                                  ? incoming_data.length
                                  : LARGE_DATA_BUFFER_SIZE;
            memcpy(large_data_buffer, incoming_data.data, copy_len);

            ESP_LOGI(TAG, "Publishing %d bytes large data", copy_len);

            // Publish full data at once
            int msg_id = esp_mqtt_client_publish(
                m_client, PUBLISH_TOPIC, (const char *)large_data_buffer,
                copy_len,
                1, // QoS 1
                0  // No retain
            );

            if (msg_id >= 0) {
              ESP_LOGI(TAG, "Successfully published %d bytes, msg_id=%d",
                       copy_len, msg_id);
              ESP_LOGD(TAG, "First 64 bytes: %.*s", 64, large_data_buffer);
            } else {
              ESP_LOGE(TAG, "Failed to publish large data");
            }

            // Small delay after large publish
            vTaskDelay(pdMS_TO_TICKS(100));
          } else {
            ESP_LOGW(TAG, "MQTT not connected, dropping large data");
          }
        }
      }
    }

    // Priority 2: Check if it's time to publish telemetry
    if ((xTaskGetTickCount() - last_telemetry_time) >=
        pdMS_TO_TICKS(PUBLISH_INTERVAL)) {
      last_telemetry_time = xTaskGetTickCount();

      // Only publish telemetry if flag is set and MQTT is connected
      if (m_client && m_mqtt_connected && s_mqtt_payload_updated) {
        esp_mqtt_client_publish(m_client, PUBLISH_TOPIC, s_mqtt_payload, 0, 1,
                                0);
        ESP_LOGI(TAG, "Published telemetry: %s", s_mqtt_payload);
        s_mqtt_payload_updated = false;
      }
    }

    // Small delay to prevent busy loop
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Cleanup
  if (large_data_buffer) {
    free(large_data_buffer);
  }

  ESP_LOGI(TAG, "Unified MQTT publish task exiting");
  vTaskDelete(NULL);
}

/*
 * Re-initialize MQTT client with new config
 */
static void mqtt_reinit(void) {
  m_mqtt_connected = false;

  if (m_client) {
    ESP_LOGI(TAG, "Stopping existing MQTT client");
    esp_mqtt_client_stop(m_client);
    esp_mqtt_client_destroy(m_client);
    m_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = s_broker_uri,
                  },
          },
      .credentials =
          {
              .username = s_device_token,
          },
      .session =
          {
              .keepalive = 120,
              .disable_clean_session = 0,
          },
      .network =
          {
              .timeout_ms = 10000,
              .refresh_connection_after_ms = 0,
              .disable_auto_reconnect = false,
          },
      .buffer =
          {
              .size = 40960, // 40KB for large data
              .out_size = 40960,
          },
  };

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  if (m_client) {
    esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(m_client);
    ESP_LOGI(TAG, "MQTT client reinitialized");
  } else {
    ESP_LOGE(TAG, "Failed to reinitialize MQTT client");
  }
}

/*
 * FreeRTOS task to monitor config queue for MQTT config updates
 */
static void mqtt_config_task(void *arg) {
  mqtt_config_data_t mqtt_cfg;
  ESP_LOGI(TAG, "MQTT config task started");

  while (!mqtt_task_close) {
    if (g_mqtt_config_queue != NULL) {
      if (xQueueReceive(g_mqtt_config_queue, &mqtt_cfg, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        ESP_LOGI(TAG, "Received MQTT config from queue");
        ESP_LOGI(TAG, "Broker: %s, Port: %d, Token: %s", mqtt_cfg.broker_uri,
                 mqtt_cfg.port, mqtt_cfg.device_token);

        strncpy(s_broker_uri, mqtt_cfg.broker_uri, sizeof(s_broker_uri) - 1);
        s_broker_uri[sizeof(s_broker_uri) - 1] = '\0';

        strncpy(s_device_token, mqtt_cfg.device_token,
                sizeof(s_device_token) - 1);
        s_device_token[sizeof(s_device_token) - 1] = '\0';

        mqtt_reinit();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "MQTT config task exiting");
  vTaskDelete(NULL);
}

/*
 * Initialize MQTT client and start single unified publishing task.
 */
void mqtt_handler_task_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = s_broker_uri,
                  },
          },
      .credentials =
          {
              .username = s_device_token,
          },
      .session =
          {
              .keepalive = 120,
              .disable_clean_session = 0,
          },
      .network =
          {
              .timeout_ms = 10000,
              .refresh_connection_after_ms = 0,
              .disable_auto_reconnect = false,
          },
      .buffer =
          {
              .size = 40960, // 40KB for large data
              .out_size = 40960,
          },
  };

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 NULL);
  esp_mqtt_client_start(m_client);

  // Create queues
  if (!g_server_cmd_queue) {
    g_server_cmd_queue = xQueueCreate(8, 128);
  }

  if (!g_mqtt_publish_queue) {
    g_mqtt_publish_queue = xQueueCreate(5, sizeof(mqtt_publish_data_t));
    if (!g_mqtt_publish_queue) {
      ESP_LOGE(TAG, "Failed to create publish queue");
    }
  }

  // Start single unified publish task (increased stack for large data)
  xTaskCreate(mqtt_publish_task, "mqtt_pub_unified", 8192, NULL, 5,
              &m_pub_task);

  // Start config monitoring task
  xTaskCreate(mqtt_config_task, "mqtt_config_task", 3072, NULL, 5, NULL);

  ESP_LOGI(TAG, "MQTT handler started with unified publish task");
}

/*
 * Stop MQTT client and delete publishing task.
 */
void mqtt_handler_task_stop(void) {
  mqtt_task_close = true;
  vTaskDelay(pdMS_TO_TICKS(500));

  if (m_client) {
    esp_mqtt_client_stop(m_client);
    esp_mqtt_client_destroy(m_client);
    m_client = NULL;
  }

  if (g_mqtt_publish_queue) {
    vQueueDelete(g_mqtt_publish_queue);
    g_mqtt_publish_queue = NULL;
  }

  ESP_LOGI(TAG, "MQTT handler stopped");
}

/*
 * Build telemetry data from source to payload buffer and clear the source.
 */
void mqtt_build_telemetry_payload(char *source, size_t len) {
  memset(s_mqtt_payload, 0, sizeof(s_mqtt_payload));
  memcpy(s_mqtt_payload, source, len);
  s_mqtt_payload_updated = true;
  memset(source, 0, len);
}

/*
 * Enqueue large data for publishing via unified publish task.
 */
void mqtt_enqueue_large_data(const uint8_t *data, size_t data_len) {
  if (!g_mqtt_publish_queue) {
    ESP_LOGW(TAG, "Publish queue not initialized");
    return;
  }

  if (!data || data_len == 0) {
    ESP_LOGW(TAG, "Invalid data for enqueue");
    return;
  }

  mqtt_publish_data_t queue_data = {.type = MQTT_DATA_TYPE_LARGE,
                                    .data = (uint8_t *)data,
                                    .length = data_len};

  if (xQueueSend(g_mqtt_publish_queue, &queue_data, pdMS_TO_TICKS(100)) ==
      pdTRUE) {
    ESP_LOGI(TAG, "Enqueued %d bytes for MQTT publish", data_len);
  } else {
    ESP_LOGW(TAG, "Failed to enqueue data - queue full");
  }
}
