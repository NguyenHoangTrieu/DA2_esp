/*
 * UART Handler for ESP32S3
 */

#include "uart_handler.h"
#include "config_handler.h"
#include <string.h>

#define DEFAULT_UART_PORT_NUM UART_NUM_0
#define DEFAULT_UART_BAUD_RATE 115200
#define DEFAULT_UART_TX_PIN GPIO_NUM_43
#define DEFAULT_UART_RX_PIN GPIO_NUM_44
#define UART_BUF_SIZE 512

static const char *TAG = "uart_handler";

static bool uart_handler_running = false;

// Current UART configuration
static bool s_uart_initialized = false;

// Callback for mode switching
static uart_mode_switch_cb_t s_mode_switch_callback = NULL;

/**
 * @brief Reinitialize UART with new configuration
 */
static void uart_reinit(uart_port_t port, uint32_t baud_rate, int tx_pin,
                        int rx_pin) {
  // Delete existing driver if initialized
  if (s_uart_initialized) {
    uart_driver_delete(DEFAULT_UART_PORT_NUM);
    s_uart_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Configure UART
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
  ESP_LOGI(TAG, "UART reinitialized: Port=%d, Baud=%u, TX=%d, RX=%d",
           DEFAULT_UART_PORT_NUM, DEFAULT_UART_BAUD_RATE, DEFAULT_UART_TX_PIN,
           DEFAULT_UART_RX_PIN);
}

/**
 * @brief Check for mode switch commands (CONFIG/NORMAL)
 * @return 0=CONFIG, 1=NORMAL, -1=no command
 */
static int check_mode_command(const char *data, int len) {
  // Check for CONFIG command (case insensitive)
  if (len >= 6) {
    if (strncasecmp(data, "CONFIG", 6) == 0) {
      return 0; // CONFIG mode
    }
  }

  // Check for NORMAL command (case insensitive)
  if (len >= 6) {
    if (strncasecmp(data, "NORMAL", 6) == 0) {
      return 1; // NORMAL mode
    }
  }

  return -1; // No mode command
}

/**
 * @brief UART handler task - receives data from UART and sends to config
 * handler Now also checks for CONFIG/NORMAL commands for mode switching
 * @param arg Task argument (unused)
 */
static void uart_handler_task(void *arg) {
  uint8_t data[UART_BUF_SIZE];

  // Initialize UART with default config
  uart_reinit(DEFAULT_UART_PORT_NUM, DEFAULT_UART_BAUD_RATE,
              DEFAULT_UART_TX_PIN, DEFAULT_UART_RX_PIN);

  ESP_LOGI(TAG, "UART handler task started");
  ESP_LOGI(TAG, "- 'CONFIG' -> switch to config mode");
  ESP_LOGI(TAG, "- 'NORMAL' -> switch to normal mode");
  ESP_LOGI(TAG, "- 'CF...' -> config commands");

  while (uart_handler_running) {
    // Read UART data
    int len = uart_read_bytes(DEFAULT_UART_PORT_NUM, data, UART_BUF_SIZE - 1,
                              pdMS_TO_TICKS(100));

    if (len > 0) {
      data[len] = '\0';

      // Check for mode switch commands FIRST (before CF check)
      int mode_cmd = check_mode_command((char *)data, len);
      if (mode_cmd != -1) {
        ESP_LOGI(TAG, "Mode switch command detected: %s", (char *)data);

        // Call callback if registered
        if (s_mode_switch_callback) {
          s_mode_switch_callback(mode_cmd);

          // Send acknowledgment
          if (mode_cmd == 0) {
            uart_write_bytes(DEFAULT_UART_PORT_NUM, "OK: CONFIG mode\r\n", 17);
          } else {
            uart_write_bytes(DEFAULT_UART_PORT_NUM, "OK: NORMAL mode\r\n", 17);
          }
        } else {
          ESP_LOGW(TAG, "Mode switch callback not registered");
        }

        continue; // Skip config command processing
      }

      // Check if command starts with "CF" (config command)
      if (len >= 2 && data[0] == 'C' && data[1] == 'F') {
        ESP_LOGI(TAG, "Config command received via UART: %s", (char *)data);

        // Skip "CF" prefix and parse actual command
        if (len > 2) {
          const char *cmd_data = (const char *)(data + 2);
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
            if (g_config_handler_queue) {
              if (xQueueSend(g_config_handler_queue, &cmd,
                             pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "Command forwarded to config handler");
              } else {
                ESP_LOGW(TAG, "Failed to send to config handler queue");
              }
            }
          } else {
            ESP_LOGW(TAG, "Unknown command type in: %s", cmd_data);
          }
        }
      } else {
        // Not a config command, just log
        ESP_LOGD(TAG, "Non-config UART data: %.*s", len, data);
      }
    }
  }

  if (s_uart_initialized) {
    uart_driver_delete(DEFAULT_UART_PORT_NUM);
  }

  ESP_LOGI(TAG, "UART handler task exiting");
  vTaskDelete(NULL);
}

/**
 * @brief Start UART handler task
 */
void uart_handler_task_start(void) {
  if (uart_handler_running) {
    ESP_LOGW(TAG, "UART handler already running");
    return;
  }

  uart_handler_running = true;
  xTaskCreate(uart_handler_task, "uart_handler", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "UART handler task created");
}

/**
 * @brief Stop UART handler task
 */
void uart_handler_task_stop(void) {
  uart_handler_running = false;
  ESP_LOGI(TAG, "UART handler task stopping");
}

/**
 * @brief Register callback for mode switching
 * @param callback Function to call when CONFIG/NORMAL detected
 *                 Parameter: 0=CONFIG, 1=NORMAL
 */
void uart_handler_register_mode_callback(uart_mode_switch_cb_t callback) {
  s_mode_switch_callback = callback;
  ESP_LOGI(TAG, "Mode switch callback registered");
}
