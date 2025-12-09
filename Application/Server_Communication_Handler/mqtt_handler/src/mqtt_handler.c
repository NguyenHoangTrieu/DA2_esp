/**
 * @file mqtt_handler.c
 * @brief MQTT handler module
 */

#include "mqtt_handler.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"

#define BROKER_URI "mqtt://demo.thingsboard.io:1883"
#define DEVICE_TOKEN "38kozd1weulcnl6ytz8f"
#define PUBLISH_TOPIC "v1/devices/me/telemetry"
#define SUBSCRIBE_TOPIC "v1/devices/me/rpc/request/+"
#define ATTRIBUTE_TOPIC "v1/devices/me/attributes"
#define DATA_BUFFER_SIZE (1024)    // 1KB
#define JSON_TX_BUFFER_SIZE (1100) // 1.1KB

static const char *TAG = "mqtt_handler";

static esp_mqtt_client_handle_t m_client = NULL;
static TaskHandle_t m_pub_task = NULL;
static volatile uint8_t m_mqtt_connected = false;

QueueHandle_t g_mqtt_publish_queue = NULL;
static bool mqtt_task_close = false;

// Global MQTT config
mqtt_config_context_t g_mqtt_ctx = {.broker_uri = BROKER_URI,
                                    .device_token = DEVICE_TOKEN,
                                    .subscribe_topic = SUBSCRIBE_TOPIC,
                                    .attribute_topic = ATTRIBUTE_TOPIC,
                                    .publish_topic = PUBLISH_TOPIC};

void mqtt_receive_enqueue(const char *data, size_t len) {
  if (data == NULL || len == 0) {
    ESP_LOGW(TAG, "Invalid parameters");
    return;
  }

  // ===== Check if Config Command (starts with "CF") =====
  if (len >= 2 && data[0] == 'C' && data[1] == 'F') {
    ESP_LOGI(TAG, "Config command received from MQTT");

    // Skip "CF" prefix and send to config handler
    const char *cmd_data = data + 2;
    int cmd_len = len - 2;

    // Parse command type
    config_type_t type = config_parse_type(cmd_data, cmd_len);

    if (type != CONFIG_TYPE_UNKNOWN) {
      config_command_t cmd;
      cmd.type = type;
      cmd.data_len = cmd_len;
      memcpy(cmd.raw_data, cmd_data, cmd_len);
      cmd.raw_data[cmd_len] = '\0';

      // Send to config handler
      extern QueueHandle_t g_config_handler_queue;
      if (g_config_handler_queue && xQueueSend(g_config_handler_queue, &cmd,
                                               pdMS_TO_TICKS(100)) == pdTRUE) {
        ESP_LOGI(TAG, "Config forwarded to config handler");
      } else {
        ESP_LOGW(TAG, "Failed to send to config handler");
      }
    } else {
      ESP_LOGW(TAG, "Unknown config type");
    }

    return;
  }
  // ===== Not Config → Route to MCU LAN Handler =====

  // Expected format: [DT][handler_type(3)][length(2)][payload]
  if (len >= 7 && data[0] == 'D' && data[1] == 'T') {
    uint8_t handler_type[4] = {data[2], data[3], data[4], '\0'};
    uint16_t payload_len = ((uint16_t)data[5] << 8) | (uint8_t)data[6];

    // Map handler string to ID
    handler_id_t target_id;
    if (memcmp(handler_type, "CAN", 3) == 0) {
      target_id = HANDLER_CAN;
    } else if (memcmp(handler_type, "LOR", 3) == 0) {
      target_id = HANDLER_LORA;
    } else if (memcmp(handler_type, "ZIG", 3) == 0) {
      target_id = HANDLER_ZIGBEE;
    } else {
      ESP_LOGW(TAG, "Unknown handler: %s", handler_type);
      return;
    }

    // Forward to MCU LAN
    const uint8_t *payload = (const uint8_t *)data + 7;
    if (mcu_lan_enqueue_downlink(target_id, (uint8_t *)payload, payload_len)) {
      ESP_LOGI(TAG, "Downlink → %s (%u bytes)", handler_type, payload_len);
    } else {
      ESP_LOGW(TAG, "Downlink enqueue failed");
    }
  } else {
    ESP_LOGW(TAG, "Invalid format");
  }
}

/**
 * @brief MQTT event handler function.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(event->client, g_mqtt_ctx.attribute_topic, 1);
    esp_mqtt_client_subscribe(event->client, g_mqtt_ctx.subscribe_topic, 1);
    esp_mqtt_client_publish(event->client, g_mqtt_ctx.attribute_topic,
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

/**
 * @brief Unified FreeRTOS task to publish all data types.
 */
static void mqtt_publish_task(void *arg) {
  mqtt_publish_data_t incoming_data;
  uint8_t *data_buffer = NULL;
  uint8_t *json_tx_buffer = NULL;

  // Allocate buffers
  data_buffer = heap_caps_malloc(DATA_BUFFER_SIZE, MALLOC_CAP_8BIT);
  if (!data_buffer) {
    ESP_LOGE(TAG, "Failed to allocate data buffer");
    vTaskDelete(NULL);
    return;
  }

  json_tx_buffer = heap_caps_malloc(JSON_TX_BUFFER_SIZE, MALLOC_CAP_8BIT);
  if (!json_tx_buffer) {
    ESP_LOGE(TAG, "Failed to allocate JSON TX buffer");
    free(data_buffer);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "MQTT publish task started");

  while (!mqtt_task_close) {
    if (g_mqtt_publish_queue) {
      if (xQueueReceive(g_mqtt_publish_queue, &incoming_data,
                        pdMS_TO_TICKS(500)) == pdTRUE) {

        // Check MQTT connection
        if (!m_mqtt_connected) {
          ESP_LOGW(TAG, "MQTT not connected, dropping data");
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }

        if (!incoming_data.data || incoming_data.length == 0) {
          ESP_LOGW(TAG, "Invalid data received from queue");
          continue;
        }

        // Copy to local buffer
        size_t copy_len = (incoming_data.length < DATA_BUFFER_SIZE)
                              ? incoming_data.length
                              : DATA_BUFFER_SIZE;
        memcpy(data_buffer, incoming_data.data, copy_len);

        // Ensure null termination for string data
        if (copy_len < DATA_BUFFER_SIZE) {
          data_buffer[copy_len] = '\0';
        } else {
          data_buffer[DATA_BUFFER_SIZE - 1] = '\0';
        }

        // Wrap string data in JSON format
        int json_len = snprintf((char *)json_tx_buffer, JSON_TX_BUFFER_SIZE,
                                "{\"data\":\"%s\"}", (char *)data_buffer);

        if (json_len >= JSON_TX_BUFFER_SIZE) {
          ESP_LOGW(TAG, "JSON buffer overflow, truncating data");
          json_len = JSON_TX_BUFFER_SIZE - 1;
          json_tx_buffer[json_len] = '\0';
        }

        ESP_LOGI(TAG, "Publishing %d bytes JSON", json_len);
        int msg_id = esp_mqtt_client_publish(m_client, g_mqtt_ctx.publish_topic,
                                             (const char *)json_tx_buffer,
                                             json_len, // JSON length
                                             1,        // QoS 1
                                             0         // No retain
        );

        if (msg_id >= 0) {
          ESP_LOGI(TAG, "Successfully published JSON, msg_id=%d", msg_id);
        } else {
          ESP_LOGE(TAG, "Failed to publish data");
        }

        // Delay to prevent TCP overflow
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  if (data_buffer)
    free(data_buffer);
  if (json_tx_buffer)
    free(json_tx_buffer);

  ESP_LOGI(TAG, "MQTT publish task exiting");
  m_pub_task = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief Re-initialize MQTT client with new config.
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
                      .uri = g_mqtt_ctx.broker_uri,
                  },
          },
      .credentials =
          {
              .username = g_mqtt_ctx.device_token,
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
              .size = 65536, // 64KB
              .out_size = 65536,
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

/**
 * @brief FreeRTOS task to monitor config queue.
 */
static void mqtt_config_task(void *arg) {
  mqtt_config_data_t mqtt_cfg;
  ESP_LOGI(TAG, "MQTT config task started");

  while (!mqtt_task_close) {
    if (g_mqtt_config_queue != NULL) {
      if (xQueueReceive(g_mqtt_config_queue, &mqtt_cfg, pdMS_TO_TICKS(500)) ==
          pdTRUE) {
        ESP_LOGI(TAG, "Received MQTT config from queue");
        ESP_LOGI(TAG, "Broker: %s, Token: %s", mqtt_cfg.broker_uri,
                 mqtt_cfg.device_token);
        ESP_LOGI(TAG, "Publish: %s, Subscribe: %s, Attribute: %s",
                 mqtt_cfg.publish_topic, mqtt_cfg.subscribe_topic,
                 mqtt_cfg.attribute_topic);

        // Update all config fields
        strncpy(g_mqtt_ctx.broker_uri, mqtt_cfg.broker_uri,
                sizeof(g_mqtt_ctx.broker_uri) - 1);
        g_mqtt_ctx.broker_uri[sizeof(g_mqtt_ctx.broker_uri) - 1] = '\0';

        strncpy(g_mqtt_ctx.device_token, mqtt_cfg.device_token,
                sizeof(g_mqtt_ctx.device_token) - 1);
        g_mqtt_ctx.device_token[sizeof(g_mqtt_ctx.device_token) - 1] = '\0';

        strncpy(g_mqtt_ctx.subscribe_topic, mqtt_cfg.subscribe_topic,
                sizeof(g_mqtt_ctx.subscribe_topic) - 1);
        g_mqtt_ctx.subscribe_topic[sizeof(g_mqtt_ctx.subscribe_topic) - 1] =
            '\0';

        strncpy(g_mqtt_ctx.attribute_topic, mqtt_cfg.attribute_topic,
                sizeof(g_mqtt_ctx.attribute_topic) - 1);
        g_mqtt_ctx.attribute_topic[sizeof(g_mqtt_ctx.attribute_topic) - 1] =
            '\0';

        strncpy(g_mqtt_ctx.publish_topic, mqtt_cfg.publish_topic,
                sizeof(g_mqtt_ctx.publish_topic) - 1);
        g_mqtt_ctx.publish_topic[sizeof(g_mqtt_ctx.publish_topic) - 1] = '\0';
        save_mqtt_config_to_nvs();
        mqtt_reinit();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "MQTT config task exiting");
  vTaskDelete(NULL);
}

/**
 * @brief Initialize MQTT client and start publishing task.
 */
void mqtt_handler_task_start(void) {
  mqtt_task_close = false;

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = g_mqtt_ctx.broker_uri,
                  },
          },
      .credentials =
          {
              .username = g_mqtt_ctx.device_token,
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
              .size = 65536,
              .out_size = 65536,
          },
  };

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                 NULL);
  esp_mqtt_client_start(m_client);

  if (!g_mqtt_publish_queue) {
    g_mqtt_publish_queue = xQueueCreate(10, sizeof(mqtt_publish_data_t));
    if (!g_mqtt_publish_queue) {
      ESP_LOGE(TAG, "Failed to create publish queue");
    }
  }

  // Start publish task
  if (m_pub_task == NULL) {
    xTaskCreate(mqtt_publish_task, "mqtt_pub", 8192, NULL, 5, &m_pub_task);
  }

  // Start config task
  xTaskCreate(mqtt_config_task, "mqtt_config", 3072, NULL, 5, NULL);

  ESP_LOGI(TAG, "MQTT handler started");
}

/**
 * @brief Stop MQTT client and delete tasks.
 */
void mqtt_handler_task_stop(void) {
  ESP_LOGI(TAG, "Stopping MQTT handler...");

  mqtt_task_close = true;
  m_mqtt_connected = false;

  // Wait for tasks to exit
  vTaskDelay(pdMS_TO_TICKS(1000));

  if (m_client) {
    esp_mqtt_client_stop(m_client);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_mqtt_client_destroy(m_client);
    m_client = NULL;
  }

  // Clear queue if exists
  if (g_mqtt_publish_queue) {
    mqtt_publish_data_t dummy;
    while (xQueueReceive(g_mqtt_publish_queue, &dummy, 0) == pdTRUE) {
      // Drain queue
    }
  }

  ESP_LOGI(TAG, "MQTT handler stopped");
}

/**
 * @brief Unified function to enqueue any data for publishing.
 */
bool mqtt_enqueue_telemetry(const uint8_t *data, size_t data_len) {
  if (!g_mqtt_publish_queue) {
    ESP_LOGW(TAG, "Publish queue not initialized");
    return false;
  }

  if (!data || data_len == 0) {
    ESP_LOGW(TAG, "Invalid data for enqueue");
    return false;
  }

  if (!m_mqtt_connected) {
    ESP_LOGW(TAG, "MQTT not connected, dropping data");
    return false;
  }

  mqtt_publish_data_t queue_data = {.data = (uint8_t *)data,
                                    .length = data_len};

  if (xQueueSend(g_mqtt_publish_queue, &queue_data, pdMS_TO_TICKS(100)) ==
      pdTRUE) {
    ESP_LOGI(TAG, "Enqueued %d bytes for MQTT publish", data_len);
    return true;
  } else {
    ESP_LOGW(TAG, "Failed to enqueue data - queue full");
    return false;
  }
}
