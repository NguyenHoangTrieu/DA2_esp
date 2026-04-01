/*
 * UART Handler for ESP32S3
 */

#include "uart_handler.h"
#include "config_handler.h"
#include "lte_connect.h"
#include "mcu_lan_handler.h"
#include "mqtt_handler.h"
#include "wifi_connect.h"
#include "esp_log.h"
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
extern http_config_data_t g_http_cfg;
extern coap_config_data_t g_coap_cfg;

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
 * @brief Convert TCA pin index back to label string for CFSC response
 * 11 -> "WK", 12 -> "PE", 0..10 -> "01".."11", else ""
 */
static void tca_pin_to_label(uint8_t pin, char *out, size_t size) {
  if (pin == 11)      { strncpy(out, "WK", size); }
  else if (pin == 12) { strncpy(out, "PE", size); }
  else if (pin <= 10) { snprintf(out, size, "%02d", pin + 1); }
  else                { strncpy(out, "", size); }
}

/**
 * @brief Handle CFSC (Config Scan) command
 * Combines local WAN config + remote LAN config (via SPI)
 */
static void handle_cfsc_command(void) {
  ESP_LOGI(TAG, "Processing CFSC (scan) command");

  // Suppress all ESP log output for the duration of the CFSC response so that
  // debug messages from other tasks (MCU_LAN_UL, MCU_LAN_DL, etc.) do not
  // interleave with the structured data sent to the PC app on the same UART.
  esp_log_level_set("*", ESP_LOG_NONE);

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
    uart_send_kv("lte_modem_name", lte_cfg.modem_name);
    char pwr_pin_str[4] = {0};
    char rst_pin_str[4] = {0};
    tca_pin_to_label(lte_cfg.pwr_pin, pwr_pin_str, sizeof(pwr_pin_str));
    tca_pin_to_label(lte_cfg.rst_pin, rst_pin_str, sizeof(rst_pin_str));
    uart_send_kv("lte_pwr_pin", pwr_pin_str);
    uart_send_kv("lte_rst_pin", rst_pin_str);
  } else {
    uart_send_kv("lte_apn", "ERROR:MUTEX_TIMEOUT");
  }

  // Server settings
  const char *srv_type_str = (g_server_type == CONFIG_SERVERTYPE_MQTT) ? "MQTT"
                             : (g_server_type == CONFIG_SERVERTYPE_COAP) ? "COAP"
                             : (g_server_type == CONFIG_SERVERTYPE_HTTP) ? "HTTP"
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
    uart_send_kv_int("mqtt_keepalive_s", mqtt_cfg.keepalive_s);
    uart_send_kv_ulong("mqtt_timeout_ms", mqtt_cfg.timeout_ms);
  } else {
    uart_send_kv("mqtt_broker", "ERROR:MUTEX_TIMEOUT");
  }

  // HTTP settings (plain global — only written by config_handler task)
  uart_send_kv("http_url",           g_http_cfg.server_url);
  uart_send_kv("http_auth_token",
               strlen(g_http_cfg.auth_token) > 0 ? "***HIDDEN***" : "");
  uart_send_kv_int("http_port",      g_http_cfg.port);
  uart_send_kv_int("http_use_tls",   g_http_cfg.use_tls ? 1 : 0);
  uart_send_kv_int("http_verify_server", g_http_cfg.verify_server ? 1 : 0);
  uart_send_kv_ulong("http_timeout_ms", g_http_cfg.timeout_ms);

  // CoAP settings (plain global — only written by config_handler task)
  uart_send_kv("coap_host",          g_coap_cfg.host);
  uart_send_kv("coap_resource_path", g_coap_cfg.resource_path);
  uart_send_kv("coap_device_token",
               strlen(g_coap_cfg.device_token) > 0 ? "***HIDDEN***" : "");
  uart_send_kv_int("coap_port",      g_coap_cfg.port);
  uart_send_kv_int("coap_use_dtls",  g_coap_cfg.use_dtls ? 1 : 0);
  uart_send_kv_ulong("coap_ack_timeout_ms", g_coap_cfg.ack_timeout_ms);
  uart_send_kv_int("coap_max_retransmit", g_coap_cfg.max_retransmit);
  uart_send_kv_ulong("coap_rpc_poll_interval_ms", g_coap_cfg.rpc_poll_interval_ms);

  // WAN hardware stack ID (identifies which LTE adapter is connected)
  uart_send_kv("stack_wan_id", config_get_wan_stack_id());

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

  // Restore log level before returning
  esp_log_level_set("*", ESP_LOG_INFO);
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
 * @brief Process one complete (newline-terminated) UART command line.
 *        line must be null-terminated and already trimmed of \r/\n.
 */
static void process_uart_line(char *line, int line_len)
{
    ESP_LOGI(TAG, "Processing line: %s (len=%d)", line, line_len);

    // Mode switch commands (CONFIG / NORMAL)
    int mode_cmd = check_mode_command(line, line_len);
    if (mode_cmd != -1) {
        ESP_LOGI(TAG, "Mode switch: %s", line);
        if (s_mode_switch_callback) {
            s_mode_switch_callback(mode_cmd);
        }
        uart_println(mode_cmd == 0 ? "OK:CONFIG_MODE" : "OK:NORMAL_MODE");
        return;
    }

    // CFSC — config scan
    if (strncasecmp(line, "CFSC", 4) == 0) {
        handle_cfsc_command();
        return;
    }

    // All other config commands start with "CF"
    if (line_len >= 2 && line[0] == 'C' && line[1] == 'F') {
        if (line_len <= 2) return; // bare "CF" with no payload

        const char *cmd_data = line + 2;
        int cmd_len = line_len - 2;

        if (cmd_len > (UART_BUF_SIZE - 2)) {
            ESP_LOGE(TAG, "Config too large: %d bytes", cmd_len);
            uart_println("ERROR:CONFIG_TOO_LARGE");
            return;
        }

        config_type_t type = config_parse_type(cmd_data, cmd_len);
        if (type == CONFIG_TYPE_UNKNOWN) {
            ESP_LOGW(TAG, "Unknown command type for: %s", cmd_data);
            uart_println("ERROR:UNKNOWN_CMD");
            return;
        }

        config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
        if (cmd == NULL) {
            ESP_LOGE(TAG, "Failed to allocate config command buffer");
            uart_println("ERROR:OUT_OF_MEMORY");
            return;
        }

        cmd->type     = type;
        cmd->data_len = cmd_len;
        cmd->source   = CMD_SOURCE_UART;
        memcpy(cmd->raw_data, cmd_data, cmd_len);
        cmd->raw_data[cmd_len] = '\0';

        if (g_config_handler_queue) {
            if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "Command forwarded to config handler");
                uart_println("OK:CMD_QUEUED");
            } else {
                ESP_LOGW(TAG, "Config queue full");
                uart_println("ERROR:QUEUE_FULL");
                free(cmd);
            }
        } else {
            uart_println("ERROR:NO_HANDLER");
            free(cmd);
        }
    } else {
        uart_println("ERROR:INVALID_CMD");
        ESP_LOGD(TAG, "Invalid command: %s", line);
    }
}

/**
 * @brief UART handler task
 *
 * Reads raw bytes from UART and assembles them into lines (delimited by '\n').
 * Each complete line is dispatched to process_uart_line(), so multiple commands
 * packed into a single UART transfer (e.g. "CFSV:0\r\nCFMQ:...\r\n") are all
 * processed correctly instead of being treated as one command.
 */
static void uart_handler_task(void *arg) {
  // Raw receive buffer
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
  if (data == NULL) {
    ESP_LOGE(TAG, "Failed to allocate UART rx buffer (%d bytes)", UART_BUF_SIZE);
    vTaskDelete(NULL);
    return;
  }

  // Line-assembly buffer — accumulates bytes until '\n'
  char *line_buf = (char *)malloc(UART_BUF_SIZE);
  if (line_buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate UART line buffer (%d bytes)", UART_BUF_SIZE);
    free(data);
    vTaskDelete(NULL);
    return;
  }
  int line_len = 0;

  uart_reinit(DEFAULT_UART_PORT_NUM, DEFAULT_UART_BAUD_RATE,
              DEFAULT_UART_TX_PIN, DEFAULT_UART_RX_PIN);

  ESP_LOGI(TAG, "UART handler task started");
  uart_println("\r\nGateway UART Interface Ready");
  uart_println("Commands: CFSC (scan config), CONFIG, NORMAL, CF...");

  while (uart_handler_running) {
    int len = uart_read_bytes(DEFAULT_UART_PORT_NUM, data, UART_BUF_SIZE - 1,
                              pdMS_TO_TICKS(100));

    if (len <= 0) continue;

    // Feed each received byte into the line assembler
    for (int i = 0; i < len; i++) {
      char c = (char)data[i];

      if (c == '\n') {
        // End of line — null-terminate and trim trailing '\r' / spaces
        line_buf[line_len] = '\0';
        while (line_len > 0 &&
               (line_buf[line_len - 1] == '\r' || line_buf[line_len - 1] == ' ')) {
          line_buf[--line_len] = '\0';
        }

        if (line_len > 0) {
          process_uart_line(line_buf, line_len);
        }

        // Reset for next line
        line_len = 0;

      } else if (c != '\r') {
        // Accumulate (ignore bare '\r')
        if (line_len < UART_BUF_SIZE - 1) {
          line_buf[line_len++] = c;
        } else {
          ESP_LOGE(TAG, "UART line buffer overflow, discarding %d bytes", line_len);
          uart_println("ERROR:CMD_TOO_LARGE");
          line_len = 0;
        }
      }
    }
  }

  free(data);
  free(line_buf);
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
