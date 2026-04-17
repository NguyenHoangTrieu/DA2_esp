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
#define FIRMWARE_STATUS_TOPIC "v1/devices/me/attributes"
#define DATA_BUFFER_SIZE (1024) // 1KB
#define JSON_TX_BUFFER_SIZE (3072)
#define HEX_STRING_BUFFER_SIZE (DATA_BUFFER_SIZE * 2 + 1)

static const char *TAG = "mqtt_handler";

static esp_mqtt_client_handle_t m_client = NULL;
static TaskHandle_t m_pub_task = NULL;
static volatile uint8_t m_mqtt_connected = false;

QueueHandle_t g_mqtt_publish_queue = NULL;
static bool mqtt_task_close = false;
static int g_last_rpc_id = -1;


// Global MQTT config
mqtt_config_context_t g_mqtt_ctx = {.broker_uri = BROKER_URI,
                                    .device_token = DEVICE_TOKEN,
                                    .subscribe_topic = SUBSCRIBE_TOPIC,
                                    .attribute_topic = ATTRIBUTE_TOPIC,
                                    .publish_topic = PUBLISH_TOPIC};


/**
 * @brief Convert binary data to hex string
 * @param data Binary data
 * @param len Data length
 * @param hex_str Output hex string buffer
 * @param hex_buf_size Buffer size
 */
static void binary_to_hex_string(const uint8_t *data, size_t len, char *hex_str,
                                 size_t hex_buf_size) {
  size_t hex_len = 0;
  for (size_t i = 0; i < len && hex_len < (hex_buf_size - 3); i++) {
    hex_len +=
        snprintf(&hex_str[hex_len], hex_buf_size - hex_len, "%02X", data[i]);
  }
  hex_str[hex_len] = '\0';
}

/**
 * @brief Publish firmware update status to ThingsBoard
 * @param status Status string ("success", "failed", "in_progress")
 * @param fw_version Current firmware version
 */
void mqtt_publish_firmware_status(const char *status, const char *fw_version) {
  if (!m_mqtt_connected || !m_client) {
    ESP_LOGW(TAG, "MQTT not connected, cannot publish firmware status");
    return;
  }

  char payload[256];
  int len =
      snprintf(payload, sizeof(payload),
               "{\"fw_state\":\"%s\",\"fw_version\":\"%s\"}",
               status, fw_version);

  if (len >= sizeof(payload)) {
    ESP_LOGW(TAG, "Firmware status payload truncated");
    return;
  }

  int msg_id = esp_mqtt_client_publish(m_client, g_mqtt_ctx.attribute_topic, payload,
                                       len, 1, 0);
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "✓ Firmware status published: %s", status);
  } else {
    ESP_LOGE(TAG, "✗ Failed to publish firmware status");
  }
}

/**
 * @brief Convert hex string to binary
 * @param hex_str Input hex string (e.g., "43464D4C3A434646")
 * @param binary Output binary buffer
 * @param binary_size Output buffer size
 * @return Number of bytes written
 */
static size_t hex_string_to_binary(const char *hex_str, uint8_t *binary,
                                   size_t binary_size) {
  size_t hex_len = strlen(hex_str);
  size_t binary_len = 0;

  // Skip spaces if any
  for (size_t i = 0; i < hex_len && binary_len < binary_size;) {
    if (hex_str[i] == ' ') {
      i++;
      continue;
    }

    // Need at least 2 characters for a byte
    if (i + 1 >= hex_len)
      break;

    char hex_byte[3] = {hex_str[i], hex_str[i + 1], '\0'};
    unsigned int byte_val;

    if (sscanf(hex_byte, "%02X", &byte_val) == 1) {
      binary[binary_len++] = (uint8_t)byte_val;
    }

    i += 2;
  }

  return binary_len;
}

void mqtt_receive_enqueue(const char *data, size_t len) {
  if (data == NULL || len == 0) {
    return;
  }

  // ===== Convert hex string to binary =====
  uint8_t binary_data[512];
  size_t binary_len =
      hex_string_to_binary(data, binary_data, sizeof(binary_data));

  if (binary_len == 0) {
    ESP_LOGW(TAG, "Failed to decode hex string");
    return;
  }

  // ===== Parse binary header =====

  // 1. Check for Config Command: "CF" (0x43 0x46)
  if (binary_len >= 2 && binary_data[0] == 0x43 && binary_data[1] == 0x46) {
    // Format: CF + config_text

    // Extract config data (skip "CF" prefix = 2 bytes)
    const char *cmd_data = (const char *)&binary_data[2];
    int cmd_len = binary_len - 2;

    if (cmd_len <= 0) {
      ESP_LOGW(TAG, "Config command has no data");
      return;
    }

    // Parse command type from first 2 chars (ML, WF, MQ, etc.)
    config_type_t type = config_parse_type(cmd_data, cmd_len);

    if (type != CONFIG_TYPE_UNKNOWN) {
      config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
      if (cmd == NULL) {
        ESP_LOGE(TAG, "Failed to allocate config command buffer");
        return;
      }
      cmd->type = type;
      cmd->data_len = cmd_len;
      cmd->source = CMD_SOURCE_MQTT;  // MQTT commands → route response to server
      memcpy(cmd->raw_data, cmd_data, cmd_len);
      cmd->raw_data[cmd_len] = '\0';

      extern QueueHandle_t g_config_handler_queue;
      if (g_config_handler_queue && xQueueSend(g_config_handler_queue, &cmd,
                                               pdMS_TO_TICKS(100)) == pdTRUE) {
        ESP_LOGI(TAG, "✓ Config forwarded to handler: %.*s", cmd_len, cmd_data);
      } else {
        ESP_LOGW(TAG, "Failed to send to config handler");
        free(cmd);
      }
    } else {
      ESP_LOGW(TAG, "Unknown config type: %.*s", cmd_len, cmd_data);
    }
    return;
  }

  // 2. Check for Data Command: "DT" (0x44 0x54)
  if (binary_len >= 7 && binary_data[0] == 0x44 && binary_data[1] == 0x54) {
    // Extract handler type (3 bytes after "DT")
    uint8_t handler_type[4] = {binary_data[2], binary_data[3], binary_data[4],
                               '\0'};

    // Extract payload length (2 bytes, big-endian)
    uint16_t payload_len = ((uint16_t)binary_data[5] << 8) | binary_data[6];

    // Map to handler ID
    handler_id_t target_id;
    if (memcmp(handler_type, "ZIG", 3) == 0) {
      target_id = HANDLER_ZIGBEE;
    } else if (memcmp(handler_type, "LOR", 3) == 0) {
      target_id = HANDLER_LORA;
    } else if (memcmp(handler_type, "CAN", 3) == 0) {
      target_id = HANDLER_CAN;
    } else if (memcmp(handler_type, "RS4", 3) == 0) {
      target_id = HANDLER_RS485;
    } else {
      ESP_LOGW(TAG, "Unknown handler: %s", handler_type);
      return;
    }

    // Extract actual payload (skip DT + handler + length = 7 bytes)
    const uint8_t *payload = &binary_data[7];
    uint16_t actual_payload_len = binary_len - 7;

    // Forward to MCU LAN
    if (mcu_lan_enqueue_downlink(target_id, (uint8_t *)payload,
                                 actual_payload_len)) {
      ESP_LOGI(TAG, "✓ Downlink → %s (%u bytes)", handler_type,
               actual_payload_len);
    } else {
      ESP_LOGW(TAG, "✗ Downlink enqueue failed");
    }
    return;
  }

  ESP_LOGW(TAG, "Unknown command format (not CF or DT)");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, binary_data, binary_len, ESP_LOG_WARN);
}

/**
 * @brief Extract hex string from JSON params
 */
static bool extract_params_from_json(const char *json, size_t json_len,
                                     char *hex_out, size_t hex_out_size) {
  // Find "params":"
  const char *params_start = strstr(json, "\"params\":\"");
  if (params_start == NULL) {
    // Try without space
    params_start = strstr(json, "\"params\":\"");
    if (params_start == NULL) {
      return false;
    }
  }

  // Skip to hex string start
  params_start += strlen("\"params\":\"");

  // Find closing quote
  const char *params_end = strchr(params_start, '"');
  if (params_end == NULL) {
    return false;
  }

  // Calculate length
  size_t hex_len = params_end - params_start;
  if (hex_len == 0 || hex_len >= hex_out_size) {
    return false;
  }

  // Copy hex string
  memcpy(hex_out, params_start, hex_len);
  hex_out[hex_len] = '\0';

  return true;
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
    // Set flags IMMEDIATELY — never call vTaskDelay inside an event handler:
    // blocking the MQTT task prevents SUBACK/PUBACK from being processed,
    // causing the broker to reset the TCP connection (errno=128 ECONNRESET).
    m_mqtt_connected = true;
    mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
    esp_mqtt_client_subscribe(event->client, g_mqtt_ctx.attribute_topic, 1);
    esp_mqtt_client_subscribe(event->client, g_mqtt_ctx.subscribe_topic, 1);
    esp_mqtt_client_publish(event->client, g_mqtt_ctx.attribute_topic,
                            "{\"chip_type\":\"esp32s3\", \"fw\":\"1.0.1\"}", 0,
                            1, 0);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    m_mqtt_connected = false;
    mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
    m_mqtt_connected = false;
    mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
    break;

  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
    ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

    // Extract RPC Request ID from topic if it is an RPC request
    char topic_buf[128];
    snprintf(topic_buf, sizeof(topic_buf), "%.*s",
             event->topic_len < 127 ? event->topic_len : 127, event->topic);
    int rpc_id = -1;
    if (sscanf(topic_buf, "v1/devices/me/rpc/request/%d", &rpc_id) == 1) {
      g_last_rpc_id = rpc_id;
      ESP_LOGI(TAG, "Stored RPC request ID: %d", g_last_rpc_id);
    }

    // Check if data is JSON (starts with '{')
    if (event->data_len > 0 && event->data[0] == '{') {
      // Try to extract params from JSON
      char hex_string[512];
      if (extract_params_from_json(event->data, event->data_len, hex_string,
                                   sizeof(hex_string))) {
        ESP_LOGI(TAG, "Extracted params: %s", hex_string);
        mqtt_receive_enqueue(hex_string, strlen(hex_string));
      } else {
        ESP_LOGW(TAG, "Failed to extract params from JSON");
      }
    } else {
      // Not JSON, pass raw data
      mqtt_receive_enqueue(event->data, event->data_len);
    }
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
  char *hex_string_buffer = NULL;

  // Allocate buffers
  data_buffer = heap_caps_malloc(DATA_BUFFER_SIZE, MALLOC_CAP_8BIT);
  json_tx_buffer = heap_caps_malloc(JSON_TX_BUFFER_SIZE, MALLOC_CAP_8BIT);
  hex_string_buffer = heap_caps_malloc(HEX_STRING_BUFFER_SIZE, MALLOC_CAP_8BIT);

  if (!data_buffer || !json_tx_buffer || !hex_string_buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    goto cleanup;
  }

  ESP_LOGI(TAG, "MQTT publish task started");
  // ===== PUBLISH FIRMWARE UPDATE SUCCESS STATUS =====
  int retry_count = 0;
  while (!m_mqtt_connected && retry_count < 500) {
    vTaskDelay(pdMS_TO_TICKS(100));
    retry_count++;
  }

  if (m_mqtt_connected) {
    // Publish firmware update success
    mqtt_publish_firmware_status("success", "1.1.1");
    ESP_LOGI(TAG, "Firmware update notification sent");
  } else {
    ESP_LOGW(TAG, "MQTT not connected, skipping firmware status");
  }

  while (!mqtt_task_close) {
    if (g_mqtt_publish_queue) {
      if (xQueueReceive(g_mqtt_publish_queue, &incoming_data,
                        pdMS_TO_TICKS(500)) == pdTRUE) {

        // Wait for MQTT reconnection instead of dropping — esp-mqtt auto-reconnects
        if (!m_mqtt_connected) {
          ESP_LOGW(TAG, "MQTT not connected, waiting up to 10s for reconnect...");
          int wait_ms = 0;
          while (!m_mqtt_connected && wait_ms < 10000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            wait_ms += 200;
          }
          if (!m_mqtt_connected) {
            ESP_LOGE(TAG, "MQTT reconnect timeout (10s), discarding queued item");
            continue;
          }
          ESP_LOGI(TAG, "MQTT reconnected after %d ms, retrying publish", wait_ms);
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

        // ===== Convert binary to hex string =====
        binary_to_hex_string(data_buffer, copy_len, hex_string_buffer,
                             HEX_STRING_BUFFER_SIZE);

        // Wrap hex string in JSON
        int json_len = snprintf((char *)json_tx_buffer, JSON_TX_BUFFER_SIZE,
                                "{\"data\":\"%s\"}", hex_string_buffer);

        if (json_len >= JSON_TX_BUFFER_SIZE) {
          ESP_LOGW(TAG, "JSON buffer overflow");
          json_len = JSON_TX_BUFFER_SIZE - 1;
          json_tx_buffer[json_len] = '\0';
        }

        ESP_LOGI(TAG,
                 "Publishing %d bytes JSON (raw: %d bytes → hex: %d chars)",
                 json_len, copy_len, strlen(hex_string_buffer));

        int msg_id;
        if (g_last_rpc_id >= 0) {
          /* Derive response topic from subscribe topic: strip "/request/+" → append "/response/{id}" */
          char base_topic[128];
          strncpy(base_topic, g_mqtt_ctx.subscribe_topic, sizeof(base_topic) - 1);
          base_topic[sizeof(base_topic) - 1] = '\0';
          char *req_pos = strstr(base_topic, "/request");
          if (req_pos) *req_pos = '\0';
          /* rpc_topic must fit: base(≤127) + "/response/" (10) + int(≤11) + NUL = 149 max */
          char rpc_topic[160];
          snprintf(rpc_topic, sizeof(rpc_topic), "%s/response/%d", base_topic, g_last_rpc_id);
          g_last_rpc_id = -1; // Consume the RPC ID
          msg_id = esp_mqtt_client_publish(m_client, rpc_topic,
                                           (const char *)json_tx_buffer,
                                           json_len, 1, 0);
          ESP_LOGI(TAG, "Routed to RPC response topic: %s", rpc_topic);
        } else {
          msg_id = esp_mqtt_client_publish(m_client, g_mqtt_ctx.publish_topic,
                                           (const char *)json_tx_buffer,
                                           json_len, 1, 0);
        }

        if (msg_id >= 0) {
          ESP_LOGI(TAG, "✓ Published, msg_id=%d", msg_id);
        } else {
          ESP_LOGE(TAG, "✗ Publish failed");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

cleanup:
  if (data_buffer)
    free(data_buffer);
  if (json_tx_buffer)
    free(json_tx_buffer);
  if (hex_string_buffer)
    free(hex_string_buffer);

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

  uint16_t ka = g_mqtt_ctx.keepalive_s ? g_mqtt_ctx.keepalive_s : 30;
  uint32_t tmo = g_mqtt_ctx.timeout_ms  ? g_mqtt_ctx.timeout_ms  : 30000;

  /* Enforce short keepalive for cellular links to bypass aggressive carrier NAT timeouts */
  if (g_internet_type == CONFIG_INTERNET_LTE && ka > 30) {
      ESP_LOGW(TAG, "Clamping MQTT keepalive from %u to 30s for LTE stability", ka);
      ka = 30;
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
              .keepalive = (int)ka,
              .disable_clean_session = 0,
          },
      .network =
          {
              .timeout_ms = (int)tmo,
              .refresh_connection_after_ms = 0,
              .disable_auto_reconnect = false,
              .reconnect_timeout_ms = 30000,
          },
      .buffer =
          {
              .size = 65536,
              .out_size = 65536,
          },
  };

  m_client = esp_mqtt_client_init(&mqtt_cfg);
  if (m_client) {
    esp_mqtt_client_register_event(m_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(m_client);
    ESP_LOGI(TAG, "MQTT client reinitialized (keepalive=%us, timeout=%lums)",
             ka, (unsigned long)tmo);
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

        // Update optional session params (keep existing value if new one is 0)
        if (mqtt_cfg.keepalive_s) g_mqtt_ctx.keepalive_s = mqtt_cfg.keepalive_s;
        if (mqtt_cfg.timeout_ms)  g_mqtt_ctx.timeout_ms  = mqtt_cfg.timeout_ms;

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

  uint16_t ka = g_mqtt_ctx.keepalive_s ? g_mqtt_ctx.keepalive_s : 30;
  uint32_t tmo = g_mqtt_ctx.timeout_ms  ? g_mqtt_ctx.timeout_ms  : 30000;

  /* Enforce short keepalive for cellular links to bypass aggressive carrier NAT timeouts */
  if (g_internet_type == CONFIG_INTERNET_LTE && ka > 30) {
      ESP_LOGW(TAG, "Clamping MQTT keepalive from %u to 30s for LTE stability", ka);
      ka = 30;
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
              .keepalive = (int)ka,
              .disable_clean_session = 0,
          },
      .network =
          {
              .timeout_ms = (int)tmo,
              .refresh_connection_after_ms = 0,
              .disable_auto_reconnect = false,
              .reconnect_timeout_ms = 30000,
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
    StackType_t *pub_stack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *pub_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pub_stack && pub_tcb) {
        m_pub_task = xTaskCreateStatic(mqtt_publish_task, "mqtt_pub", 8192, NULL, 5, pub_stack, pub_tcb);
    } else {
        ESP_LOGE(TAG, "Failed to allocate mqtt_pub task in PSRAM");
    }
  }

  // Start config task
  {
      StackType_t *cfg_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      StaticTask_t *cfg_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (cfg_stack && cfg_tcb) {
          xTaskCreateStatic(mqtt_config_task, "mqtt_config", 3072, NULL, 5, cfg_stack, cfg_tcb);
      } else {
          ESP_LOGE(TAG, "Failed to allocate mqtt_config task in PSRAM");
      }
  }

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

  // Clear and DELETE the queue so its internal-RAM storage is returned to
  // the heap before the next protocol handler (e.g. CoAP) tries to allocate.
  if (g_mqtt_publish_queue) {
    mqtt_publish_data_t dummy;
    while (xQueueReceive(g_mqtt_publish_queue, &dummy, 0) == pdTRUE) {
      // Drain queue
    }
    vQueueDelete(g_mqtt_publish_queue);
    g_mqtt_publish_queue = NULL;
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

  // Do NOT gate on m_mqtt_connected — esp-mqtt auto-reconnects and the publish
  // task will wait for reconnection before sending.  Dropping here causes permanent
  // data loss during transient (1-2s) disconnects.
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
