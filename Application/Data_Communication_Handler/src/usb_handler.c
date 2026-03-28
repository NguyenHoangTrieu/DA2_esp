/*
 * USB Handler for ESP32S3
 */

#include "usb_handler.h"
#include "config_handler.h"
#include "lte_connect.h"
#include "mcu_lan_handler.h"
#include "mqtt_handler.h"
#include "wifi_connect.h"
#include "esp_log.h"

#define BUF_SIZE (8192)
const char *TAG = "USB_HANDLER";

static TaskHandle_t jtag_task_hdl = NULL;
static usb_serial_jtag_driver_config_t usb_serial_jtag_config;
static bool close_jtag_task = false;
static bool usb_driver_installed = false;

// External global variables
extern wifi_config_context_t g_wifi_ctx;
extern lte_config_context_t g_lte_ctx;
extern mqtt_config_context_t g_mqtt_ctx;
extern config_internet_type_t g_internet_type;
extern config_server_type_t g_server_type;

#define GATEWAY_MODEL "ESP32S3_IoT_Gateway"
#define GATEWAY_FW_VERSION "v1.2.0"
#define GATEWAY_HW_VERSION "HW_v2.0"
#define GATEWAY_SERIAL "GW2025001"

/**
 * @brief Send string to USB
 */
static void usb_print(const char *str) {
  usb_serial_jtag_write_bytes(str, strlen(str), 20 / portTICK_PERIOD_MS);
}

/**
 * @brief Send string with newline
 */
static void usb_println(const char *str) {
  usb_print(str);
  usb_print("\r\n");
}

/**
 * @brief Format and send key=value pair
 */
static void usb_send_kv(const char *key, const char *value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%s", key, value);
  usb_println(buffer);
}

/**
 * @brief Format and send key=value pair (integer)
 */
static void usb_send_kv_int(const char *key, int value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%d", key, value);
  usb_println(buffer);
}

/**
 * @brief Format and send key=value pair (unsigned long)
 */
static void usb_send_kv_ulong(const char *key, unsigned long value) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s=%lu", key, value);
  usb_println(buffer);
}

/**
 * @brief Convert TCA pin index back to label string for CFSC response
 * 11 -> "WK", 12 -> "PE", 0..10 -> "01".."11", else ""
 */
static void tca_pin_to_label_usb(uint8_t pin, char *out, size_t size) {
  if (pin == 11)      { strncpy(out, "WK", size); }
  else if (pin == 12) { strncpy(out, "PE", size); }
  else if (pin <= 10) { snprintf(out, size, "%02d", pin + 1); }
  else                { strncpy(out, "", size); }
}

/**
 * @brief Handle CFSC (Config Scan) command via USB
 * Combines local WAN config + remote LAN config (via SPI)
 */
static void handle_cfsc_command_usb(void) {
  ESP_LOGI(TAG, "Processing CFSC (scan) command via USB");

  // Suppress all ESP log output for the duration of the CFSC response so that
  // debug messages from other tasks do not interleave with the structured data.
  esp_log_level_set("*", ESP_LOG_NONE);

  // Start marker
  usb_println("CFSC_RESP:START");

  // ==================== GATEWAY INFO ====================
  usb_println("[GATEWAY_INFO]");
  usb_send_kv("model", GATEWAY_MODEL);
  usb_send_kv("firmware", GATEWAY_FW_VERSION);
  usb_send_kv("hardware", GATEWAY_HW_VERSION);
  usb_send_kv("serial", GATEWAY_SERIAL);

  // Internet status
  internet_status_t inet_status = mcu_lan_handler_get_internet_status();
  usb_send_kv("internet_status",
              inet_status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");

  // RTC time
  char rtc_buffer[20] = {0};
  if (mcu_lan_handler_get_rtc(rtc_buffer) == ESP_OK) {
    usb_send_kv("rtc_time", rtc_buffer);
  } else {
    usb_send_kv("rtc_time", "UNAVAILABLE");
  }

  // ==================== WAN CONFIG (LOCAL) ====================
  usb_println("");
  usb_println("[WAN_CONFIG]");

  // Internet type
  const char *inet_type_str = (g_internet_type == CONFIG_INTERNET_WIFI) ? "WIFI"
                              : (g_internet_type == CONFIG_INTERNET_LTE)
                                  ? "LTE"
                                  : "UNKNOWN";
  usb_send_kv("internet_type", inet_type_str);

  // WiFi settings - thread-safe read
  wifi_config_context_t wifi_cfg;
  if (config_get_wifi_safe(&wifi_cfg) == ESP_OK) {
    usb_send_kv("wifi_ssid", wifi_cfg.ssid);
    usb_send_kv("wifi_password",
                strlen(wifi_cfg.pass) > 0 ? "***HIDDEN***" : "");
    usb_send_kv("wifi_username", wifi_cfg.username);
    usb_send_kv_int("wifi_auth_mode", wifi_cfg.auth_mode);
  } else {
    usb_send_kv("wifi_ssid", "ERROR:MUTEX_TIMEOUT");
    ESP_LOGE(TAG, "Failed to get WiFi config safely");
  }

  // LTE settings - thread-safe read
  lte_config_context_t lte_cfg;
  if (config_get_lte_safe(&lte_cfg) == ESP_OK) {
    usb_send_kv("lte_apn", lte_cfg.apn);
    usb_send_kv("lte_username", lte_cfg.username);
    usb_send_kv("lte_password",
                strlen(lte_cfg.password) > 0 ? "***HIDDEN***" : "");
    const char *lte_comm_str =
        (lte_cfg.comm_type == LTE_HANDLER_UART) ? "UART" : "USB";
    usb_send_kv("lte_comm_type", lte_comm_str);
    usb_send_kv_ulong("lte_max_retries", lte_cfg.max_reconnect_attempts);
    usb_send_kv_ulong("lte_timeout_ms", lte_cfg.reconnect_timeout_ms);
    usb_send_kv("lte_auto_reconnect",
                lte_cfg.auto_reconnect ? "true" : "false");
    usb_send_kv("lte_modem_name", lte_cfg.modem_name);
    char pwr_pin_str[4] = {0};
    char rst_pin_str[4] = {0};
    tca_pin_to_label_usb(lte_cfg.pwr_pin, pwr_pin_str, sizeof(pwr_pin_str));
    tca_pin_to_label_usb(lte_cfg.rst_pin, rst_pin_str, sizeof(rst_pin_str));
    usb_send_kv("lte_pwr_pin", pwr_pin_str);
    usb_send_kv("lte_rst_pin", rst_pin_str);
  } else {
    usb_send_kv("lte_apn", "ERROR:MUTEX_TIMEOUT");
  }

  // Server settings
  const char *srv_type_str = (g_server_type == CONFIG_SERVERTYPE_MQTT) ? "MQTT"
                             : (g_server_type == CONFIG_SERVERTYPE_HTTP)
                                 ? "HTTP"
                                 : "UNKNOWN";
  usb_send_kv("server_type", srv_type_str);

  // MQTT settings - thread-safe read
  mqtt_config_context_t mqtt_cfg;
  if (config_get_mqtt_safe(&mqtt_cfg) == ESP_OK) {
    usb_send_kv("mqtt_broker", mqtt_cfg.broker_uri);
    usb_send_kv("mqtt_pub_topic", mqtt_cfg.publish_topic);
    usb_send_kv("mqtt_sub_topic", mqtt_cfg.subscribe_topic);
    usb_send_kv("mqtt_device_token",
                strlen(mqtt_cfg.device_token) > 0 ? "***HIDDEN***" : "");
    usb_send_kv("mqtt_attribute_topic", mqtt_cfg.attribute_topic);
  } else {
    usb_send_kv("mqtt_broker", "ERROR:MUTEX_TIMEOUT");
  }

  // WAN hardware stack ID (identifies which LTE adapter is connected)
  usb_send_kv("stack_wan_id", config_get_wan_stack_id());

  // ==================== LAN CONFIG (FROM LAN MCU VIA SPI) ====================
  usb_println("");
  usb_println("[LAN_CONFIG]");

  // Request LAN config from LAN MCU via mcu_lan_handler (SPI Master)
  uint8_t lan_config_buffer[512] = {0};
  uint16_t lan_config_len = 0;

  esp_err_t ret = mcu_lan_handler_request_config_async(
      lan_config_buffer, &lan_config_len, sizeof(lan_config_buffer), 2000);
  if (ret == ESP_OK && lan_config_len > 0) {
    // Parse LAN config: format is "key=value|key=value|key=value"
    // Convert | to newline and send
    char *line_start = (char *)lan_config_buffer;
    char *line_end = NULL;

    while ((line_end = strchr(line_start, '|')) != NULL) {
      *line_end = '\0';          // Replace | with null terminator
      usb_println(line_start);   // Send key=value line
      line_start = line_end + 1; // Move to next line
    }

    // Send remaining data (last line without |)
    if (strlen(line_start) > 0) {
      usb_println(line_start);
    }

    ESP_LOGI(TAG, "LAN config received and sent via USB (%u bytes)",
             lan_config_len);
  } else {
    // Fallback: LAN config unavailable
    usb_send_kv("lan_status", "UNAVAILABLE");
    ESP_LOGW(TAG, "Failed to retrieve LAN config from LAN MCU");
  }

  // End marker
  usb_println("");
  usb_println("CFSC_RESP:END");

  // Restore log level before returning
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(TAG, "CFSC response completed via USB");
}

/**
 * @brief JTAG handler task
 */
static void jtag_task(void *arg) {
  if (!usb_driver_installed) {
    usb_serial_jtag_config.rx_buffer_size = BUF_SIZE;
    usb_serial_jtag_config.tx_buffer_size = BUF_SIZE;
    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    if (ret == ESP_OK) {
      usb_driver_installed = true;
      ESP_LOGI(TAG, "USB_SERIAL_JTAG driver installed");
    } else if (ret == ESP_ERR_INVALID_STATE) {
      usb_driver_installed = true;
      ESP_LOGW(TAG, "USB_SERIAL_JTAG driver already installed");
    } else {
      ESP_ERROR_CHECK(ret);
    }
  } else {
    ESP_LOGI(TAG, "USB_SERIAL_JTAG already initialized");
  }

  // Per-packet read buffer (one USB FS packet = 64 bytes max)
  uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
  if (data == NULL) {
    ESP_LOGE(TAG, "no memory for data");
    vTaskDelete(NULL);
    return;
  }

  // Line-assembly buffer: accumulates USB packets until '\n' is received.
  // USB sends data in 64-byte physical packets; a single large command
  // (e.g. CFML JSON ~5 KB) arrives across many consecutive packets.
  // Without this buffer each 64-byte fragment would be parsed as a separate
  // command, causing ERROR:INVALID_CMD for everything after the first chunk.
  char *line_buf = (char *)malloc(BUF_SIZE);
  if (line_buf == NULL) {
    ESP_LOGE(TAG, "no memory for line_buf");
    free(data);
    vTaskDelete(NULL);
    return;
  }
  int line_len = 0;

  usb_println("\r\nGateway USB Interface Ready");
  usb_println("Commands: CFSC (scan config), CF... (config commands)");

  while (!close_jtag_task) {
    int len = usb_serial_jtag_read_bytes(data, (BUF_SIZE - 1),
                                         20 / portTICK_PERIOD_MS);

    if (len <= 0)
      continue;

    // Append received bytes to the line assembly buffer
    for (int i = 0; i < len; i++) {
      char c = (char)data[i];

      if (c == '\n') {
        // Complete line received – null-terminate and process
        line_buf[line_len] = '\0';

        // Trim trailing '\r' or spaces
        while (line_len > 0 && (line_buf[line_len - 1] == '\r' ||
                                 line_buf[line_len - 1] == ' ')) {
          line_buf[--line_len] = '\0';
        }

        if (line_len == 0) {
          // Empty line, ignore
          continue;
        }

        // ---- Process one complete line ----
        {
          char *processed = line_buf;
          int plen = line_len;

          ESP_LOGI(TAG, "USB Received: %s (len=%d)", processed, plen);

          // Check for CFSC command (scan config)
          if (strncasecmp(processed, "CFSC", 4) == 0) {
            handle_cfsc_command_usb();
          }
          // Check for config commands starting with "CF"
          else if (plen >= 2 && processed[0] == 'C' && processed[1] == 'F') {
            ESP_LOGI(TAG, "Config command via USB: %s", processed);

            if (plen > 2) {
              const char *cmd_data = processed + 2;
              int cmd_len = plen - 2;

              config_type_t type = config_parse_type(cmd_data, cmd_len);
              if (type != CONFIG_TYPE_UNKNOWN) {
                config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
                if (cmd == NULL) {
                  ESP_LOGE(TAG, "Failed to allocate config command buffer");
                  usb_println("ERROR:OUT_OF_MEMORY");
                } else {
                  cmd->type = type;
                  cmd->data_len = cmd_len;
                  cmd->source = CMD_SOURCE_USB;
                  memcpy(cmd->raw_data, cmd_data, cmd_len);
                  cmd->raw_data[cmd_len] = '\0';

                  if (g_config_handler_queue) {
                    if (xQueueSend(g_config_handler_queue, &cmd,
                                   pdMS_TO_TICKS(100)) == pdTRUE) {
                      ESP_LOGI(TAG, "Command forwarded to config handler");
                      usb_println("OK:CMD_QUEUED");
                    } else {
                      ESP_LOGW(TAG, "Config queue full");
                      usb_println("ERROR:QUEUE_FULL");
                      free(cmd);
                    }
                  } else {
                    usb_println("ERROR:NO_HANDLER");
                    free(cmd);
                  }
                }
              } else {
                ESP_LOGW(TAG, "Unknown command type");
                usb_println("ERROR:UNKNOWN_CMD");
              }
            }
          } else {
            // Unknown command
            usb_println("ERROR:INVALID_CMD");
            ESP_LOGW(TAG, "Invalid command: %s", processed);
          }
        } // end process block

        // Reset line buffer for next command
        line_len = 0;

      } else if (c != '\r') {
        // Accumulate character (ignore bare '\r')
        if (line_len < BUF_SIZE - 1) {
          line_buf[line_len++] = c;
        } else {
          // Line buffer overflow → discard and reset
          ESP_LOGE(TAG, "USB line buffer overflow, discarding %d bytes", line_len);
          usb_println("ERROR:CMD_TOO_LARGE");
          line_len = 0;
        }
      }
    } // end for each byte
  } // end while

  free(data);
  free(line_buf);
  usb_serial_jtag_driver_uninstall();
  ESP_LOGI(TAG, "USB_SERIAL_JTAG deinit done");
  vTaskDelete(NULL);
}

void jtag_task_start(void) {
  /* Check if task is already running */
  if (jtag_task_hdl != NULL) {
    /* Task exists, just resume if needed */
    close_jtag_task = false;
    ESP_LOGW(TAG, "JTAG handler task already running, skipping creation");
    return;
  }

  close_jtag_task = false;
  BaseType_t task_created;

  task_created = xTaskCreatePinnedToCore(jtag_task, "jtag_handler", 1024 * 16, NULL,
                                         JTAG_TASK_PRIORITY, &jtag_task_hdl, 0);
  assert(task_created == pdTRUE);
  ESP_LOGI(TAG, "JTAG handler task created");
}

void jtag_task_stop(void) {
  close_jtag_task = true;
  ESP_LOGI(TAG, "JTAG handler task stopping");
}
