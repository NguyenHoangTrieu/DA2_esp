/*
 * UART Handler for ESP32S3
 */

#include "uart_handler.h"
#include "config_handler.h"
#include "lte_connect.h"
#include "mcu_lan_handler.h"
#include "mqtt_handler.h"
#include "wifi_connect.h"
#include <string.h>

#define DEFAULT_UART_PORT_NUM UART_NUM_0
#define DEFAULT_UART_BAUD_RATE 115200
#define DEFAULT_UART_TX_PIN GPIO_NUM_43
#define DEFAULT_UART_RX_PIN GPIO_NUM_44

static const char *TAG = "uart_handler";
static bool uart_handler_running = false;
static bool s_uart_initialized = false;
static uart_mode_switch_cb_t s_mode_switch_callback = NULL;

// External global variables from config_load_save.c (WAN)
extern wifi_config_context_t g_wifi_ctx;
extern lte_config_context_t g_lte_ctx;
extern mqtt_config_context_t g_mqtt_ctx;
extern config_internet_type_t g_internet_type;
extern config_server_type_t g_server_type;

// Gateway info
#define GATEWAY_MODEL "ESP32S3_IoT_Gateway"
#define GATEWAY_FW_VERSION "v1.2.0"
#define GATEWAY_HW_VERSION "HW_v2.0"
#define GATEWAY_SERIAL "GW2025001"

/**
 * @brief Send string to UART
 */
static void uart_print(const char *str) {
  uart_write_bytes(DEFAULT_UART_PORT_NUM, str, strlen(str));
}

/**
 * @brief Send string with newline
 */
static void uart_println(const char *str) {
  uart_print(str);
  uart_print("\r\n");
}

/**
 * @brief Format and send key=value pair
 */
static void uart_send_kv(const char *key, const char *value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%s", key, value);
  uart_println(buffer);
}

/**
 * @brief Format and send key=value pair (integer)
 */
static void uart_send_kv_int(const char *key, int value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%d", key, value);
  uart_println(buffer);
}

/**
 * @brief Format and send key=value pair (unsigned long)
 */
static void uart_send_kv_ulong(const char *key, unsigned long value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%lu", key, value);
  uart_println(buffer);
}

/**
 * @brief Handle CFSC (Config Scan) command
 * Combines local WAN config + remote LAN config (via SPI)
 */
static void handle_cfsc_command(void) {
  ESP_LOGI(TAG, "Processing CFSC (scan) command");

  // Start marker
  uart_println("CFSC_RESP:START");

  // ==================== GATEWAY INFO ====================
  uart_println("[GATEWAY_INFO]");
  uart_send_kv("model", GATEWAY_MODEL);
  uart_send_kv("firmware", GATEWAY_FW_VERSION);
  uart_send_kv("hardware", GATEWAY_HW_VERSION);
  uart_send_kv("serial", GATEWAY_SERIAL);

  // Internet status (from mcu_lan_handler which connects to LAN MCU)
  internet_status_t inet_status = mcu_lan_handler_get_internet_status();
  uart_send_kv("internet_status",
               inet_status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");

  // RTC time (from mcu_lan_handler)
  char rtc_buffer[20] = {0};
  if (mcu_lan_handler_get_rtc(rtc_buffer) == ESP_OK) {
    uart_send_kv("rtc_time", rtc_buffer);
  } else {
    uart_send_kv("rtc_time", "UNAVAILABLE");
  }

  // ==================== WAN CONFIG (LOCAL) ====================
  uart_println("");
  uart_println("[WAN_CONFIG]");

  // Internet type
  const char *inet_type_str = (g_internet_type == CONFIG_INTERNET_WIFI) ? "WIFI"
                              : (g_internet_type == CONFIG_INTERNET_LTE)
                                  ? "LTE"
                                  : "UNKNOWN";
  uart_send_kv("internet_type", inet_type_str);

  // WiFi settings - thread-safe read
  wifi_config_context_t wifi_cfg;
  if (config_get_wifi_safe(&wifi_cfg) == ESP_OK) {
    uart_send_kv("wifi_ssid", wifi_cfg.ssid);
    uart_send_kv("wifi_password",
                 strlen(wifi_cfg.pass) > 0 ? "***HIDDEN***" : "");
    uart_send_kv("wifi_username", wifi_cfg.username);
    uart_send_kv_int("wifi_auth_mode", wifi_cfg.auth_mode);
  } else {
    uart_send_kv("wifi_ssid", "ERROR:MUTEX_TIMEOUT");
    ESP_LOGE(TAG, "Failed to get WiFi config safely");
  }

  // LTE settings - thread-safe read
  lte_config_context_t lte_cfg;
  if (config_get_lte_safe(&lte_cfg) == ESP_OK) {
    uart_send_kv("lte_apn", lte_cfg.apn);
    uart_send_kv("lte_username", lte_cfg.username);
    uart_send_kv("lte_password",
                 strlen(lte_cfg.password) > 0 ? "***HIDDEN***" : "");
    const char *lte_comm_str =
        (lte_cfg.comm_type == LTE_HANDLER_UART) ? "UART" : "USB";
    uart_send_kv("lte_comm_type", lte_comm_str);
    uart_send_kv_ulong("lte_max_retries", lte_cfg.max_reconnect_attempts);
    uart_send_kv_ulong("lte_timeout_ms", lte_cfg.reconnect_timeout_ms);
    uart_send_kv("lte_auto_reconnect",
                 lte_cfg.auto_reconnect ? "true" : "false");
  } else {
    uart_send_kv("lte_apn", "ERROR:MUTEX_TIMEOUT");
  }

  // Server settings
  const char *srv_type_str = (g_server_type == CONFIG_SERVERTYPE_MQTT) ? "MQTT"
                             : (g_server_type == CONFIG_SERVERTYPE_HTTP)
                                 ? "HTTP"
                                 : "UNKNOWN";
  uart_send_kv("server_type", srv_type_str);

  // MQTT settings - thread-safe read
  mqtt_config_context_t mqtt_cfg;
  if (config_get_mqtt_safe(&mqtt_cfg) == ESP_OK) {
    uart_send_kv("mqtt_broker", mqtt_cfg.broker_uri);
    uart_send_kv("mqtt_pub_topic", mqtt_cfg.publish_topic);
    uart_send_kv("mqtt_sub_topic", mqtt_cfg.subscribe_topic);
    uart_send_kv("mqtt_device_token",
                 strlen(mqtt_cfg.device_token) > 0 ? "***HIDDEN***" : "");
    uart_send_kv("mqtt_attribute_topic", mqtt_cfg.attribute_topic);
  } else {
    uart_send_kv("mqtt_broker", "ERROR:MUTEX_TIMEOUT");
  }

  // ==================== LAN CONFIG (FROM LAN MCU VIA SPI) ====================
  uart_println("");
  uart_println("[LAN_CONFIG]");

  // Request LAN config from LAN MCU via mcu_lan_handler (SPI Master)
  uint8_t lan_config_buffer[512] = {0};
  uint16_t lan_config_len = 0;

  esp_err_t ret = mcu_lan_handler_request_config_async(
    lan_config_buffer, &lan_config_len, sizeof(lan_config_buffer), 3000);

  if (ret == ESP_OK && lan_config_len > 0) {
    // Parse LAN config: format is "key=value|key=value|key=value"
    // Convert | to newline and send
    char *line_start = (char *)lan_config_buffer;
    char *line_end = NULL;

    while ((line_end = strchr(line_start, '|')) != NULL) {
      *line_end = '\0';          // Replace | with null terminator
      uart_println(line_start);  // Send key=value line
      line_start = line_end + 1; // Move to next line
    }

    // Send remaining data (last line without |)
    if (strlen(line_start) > 0) {
      uart_println(line_start);
    }

    ESP_LOGI(TAG, "LAN config received and sent (%u bytes)", lan_config_len);
  } else {
    // Fallback: LAN config unavailable
    uart_send_kv("lan_status", "UNAVAILABLE");
    ESP_LOGW(TAG, "Failed to retrieve LAN config from LAN MCU");
  }

  // End marker
  uart_println("");
  uart_println("CFSC_RESP:END");

  ESP_LOGI(TAG, "CFSC response completed");
}

/**
 * @brief Reinitialize UART with new configuration
 */
static void uart_reinit(uart_port_t port, uint32_t baud_rate, int tx_pin,
                        int rx_pin) {
  if (s_uart_initialized) {
    uart_driver_delete(DEFAULT_UART_PORT_NUM);
    s_uart_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  uart_config_t uart_config = {
      .baud_rate = DEFAULT_UART_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_param_config(DEFAULT_UART_PORT_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(DEFAULT_UART_PORT_NUM, DEFAULT_UART_TX_PIN,
                               DEFAULT_UART_RX_PIN, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_driver_install(DEFAULT_UART_PORT_NUM, UART_BUF_SIZE * 2,
                                      0, 0, NULL, 0));
  s_uart_initialized = true;
  ESP_LOGI(TAG, "UART initialized: Baud=%u, TX=%d, RX=%d",
           DEFAULT_UART_BAUD_RATE, DEFAULT_UART_TX_PIN, DEFAULT_UART_RX_PIN);
}

/**
 * @brief Check for mode switch commands (CONFIG/NORMAL)
 */
static int check_mode_command(const char *data, int len) {
  if (len >= 6) {
    if (strncasecmp(data, "CONFIG", 6) == 0) {
      return 0; // CONFIG mode
    }
    if (strncasecmp(data, "NORMAL", 6) == 0) {
      return 1; // NORMAL mode
    }
  }
  return -1;
}

/**
 * @brief UART handler task
 */
static void uart_handler_task(void *arg) {
  // Allocate buffer on HEAP instead of stack to prevent overflow
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
  if (data == NULL) {
    ESP_LOGE(TAG, "Failed to allocate UART buffer (%d bytes)", UART_BUF_SIZE);
    vTaskDelete(NULL);
    return;
  }

  uart_reinit(DEFAULT_UART_PORT_NUM, DEFAULT_UART_BAUD_RATE,
              DEFAULT_UART_TX_PIN, DEFAULT_UART_RX_PIN);

  ESP_LOGI(TAG, "UART handler task started");
  uart_println("\r\nGateway UART Interface Ready");
  uart_println("Commands: CFSC (scan config), CONFIG, NORMAL, CF...");

  while (uart_handler_running) {
    int len = uart_read_bytes(DEFAULT_UART_PORT_NUM, data, UART_BUF_SIZE - 1,
                              pdMS_TO_TICKS(100));

    if (len > 0) {
      data[len] = '\0';
      
      // Debug: Log original received length BEFORE trimming
      int original_len = len;
      ESP_LOGI(TAG, "UART read: %d bytes (pre-trim)", original_len);

      // Trim whitespace
      while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n' ||
                         data[len - 1] == ' ')) {
        data[--len] = '\0';
      }
      
      if (len < original_len) {
        ESP_LOGI(TAG, "Trimmed %d bytes, new len=%d", original_len - len, len);
      }

      if (len == 0)
        continue;

      ESP_LOGI(TAG, "Received: %s (len=%d)", data, len);

      // Check for mode switch commands FIRST
      int mode_cmd = check_mode_command((char *)data, len);
      if (mode_cmd != -1) {
        ESP_LOGI(TAG, "Mode switch: %s", (char *)data);
        if (s_mode_switch_callback) {
          s_mode_switch_callback(mode_cmd);
        }
        uart_println(mode_cmd == 0 ? "OK:CONFIG_MODE" : "OK:NORMAL_MODE");
        continue;
      }

      // Check for CFSC command (scan config)
      if (strncasecmp((char *)data, "CFSC", 4) == 0) {
        handle_cfsc_command();
        continue;
      }

      // Check for config commands starting with "CF"
      if (len >= 2 && data[0] == 'C' && data[1] == 'F') {

        if (len > 2) {
          const char *cmd_data = (const char *)(data + 2);
          int cmd_len = len - 2;

          // CRITICAL: Validate command length BEFORE processing
          #define MAX_CONFIG_LENGTH 8190  // = UART_BUF_SIZE - 2 ("CF" prefix)
          if (cmd_len > MAX_CONFIG_LENGTH) {
            ESP_LOGE(TAG, "Config too large: %d bytes (max %d)", cmd_len, MAX_CONFIG_LENGTH);
            uart_println("ERROR:CONFIG_TOO_LARGE");
            continue;  // Skip processing oversized configs
          }

          config_type_t type = config_parse_type(cmd_data, cmd_len);
          if (type != CONFIG_TYPE_UNKNOWN) {
            // Allocate config_command_t on HEAP (4KB+ struct too large for stack)
            config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
            if (cmd == NULL) {
              ESP_LOGE(TAG, "Failed to allocate config command buffer");
              uart_println("ERROR:OUT_OF_MEMORY");
              continue;
            }

            cmd->type = type;
            cmd->data_len = cmd_len;
            cmd->source = CMD_SOURCE_UART;  // Commands from UART → route response back to UART
            memcpy(cmd->raw_data, cmd_data, cmd_len);
            cmd->raw_data[cmd_len] = '\0';

            if (g_config_handler_queue) {
              // Queue takes ownership, config_handler must free it
              // Send POINTER (not value) since queue expects config_command_t*
              if (xQueueSend(g_config_handler_queue, &cmd,
                             pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "Command forwarded to config handler");
                uart_println("OK:CMD_QUEUED");
              } else {
                ESP_LOGW(TAG, "Config queue full");
                uart_println("ERROR:QUEUE_FULL");
                free(cmd);  // Queue full, free memory
              }
            } else {
              uart_println("ERROR:NO_HANDLER");
              free(cmd);
            }
          } else {
            ESP_LOGW(TAG, "Unknown command type");
            uart_println("ERROR:UNKNOWN_CMD");
          }
        }

      } else {
        // Unknown command
        uart_println("ERROR:INVALID_CMD");
        ESP_LOGD(TAG, "Invalid command: %s", data);
      }
    }
  }


  // Cleanup: free heap buffer before exiting
  free(data);
  ESP_LOGI(TAG, "UART handler task exiting");
  vTaskDelete(NULL);
}

void uart_handler_task_start(void) {
  if (uart_handler_running) {
    ESP_LOGW(TAG, "UART handler already running");
    return;
  }

  uart_handler_running = true;
  xTaskCreate(uart_handler_task, "uart_handler", 1024 * 16, NULL, 5, NULL);
  ESP_LOGI(TAG, "UART handler task created");
}

void uart_handler_task_stop(void) {
  uart_handler_running = false;
  ESP_LOGI(TAG, "UART handler task stopping");
}

void uart_handler_register_mode_callback(uart_mode_switch_cb_t callback) {
  s_mode_switch_callback = callback;
  ESP_LOGI(TAG, "Mode switch callback registered");
}
