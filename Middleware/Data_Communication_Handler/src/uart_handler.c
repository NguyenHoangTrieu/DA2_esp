#include "config_handler.h"
#include "uart_handler.h"

// Default UART configuration
#define DEFAULT_UART_PORT_NUM UART_NUM_0
#define DEFAULT_UART_BAUD_RATE 115200
#define DEFAULT_UART_TX_PIN GPIO_NUM_43
#define DEFAULT_UART_RX_PIN GPIO_NUM_44
#define UART_BUF_SIZE 512

static const char *TAG = "uart_handler";
static bool uart_handler_running = false;

// Current UART configuration
static uart_port_t s_uart_port = DEFAULT_UART_PORT_NUM;
static uint32_t s_uart_baud = DEFAULT_UART_BAUD_RATE;
static int s_uart_tx_pin = DEFAULT_UART_TX_PIN;
static int s_uart_rx_pin = DEFAULT_UART_RX_PIN;
static bool s_uart_initialized = false;

/**
 * @brief Reinitialize UART with new configuration
 */
static void uart_reinit(uart_port_t port, uint32_t baud_rate, int tx_pin, int rx_pin) {
    // Delete existing driver if initialized
    if (s_uart_initialized) {
        uart_driver_delete(s_uart_port);
        s_uart_initialized = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Update configuration
    s_uart_port = port;
    s_uart_baud = baud_rate;
    s_uart_tx_pin = tx_pin;
    s_uart_rx_pin = rx_pin;
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = s_uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    
    ESP_ERROR_CHECK(uart_param_config(s_uart_port, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_port, s_uart_tx_pin, s_uart_rx_pin, 
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(s_uart_port, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    
    s_uart_initialized = true;
    ESP_LOGI(TAG, "UART reinitialized: Port=%d, Baud=%u, TX=%d, RX=%d", 
             s_uart_port, s_uart_baud, s_uart_tx_pin, s_uart_rx_pin);
}

/**
 * @brief UART handler task - receives data from UART and sends to config handler
 * Checks for "CF" prefix to identify configuration commands
 * @param arg Task argument (unused)
 */
static void uart_handler_task(void *arg) {
    uint8_t data[UART_BUF_SIZE];
    
    // Initialize UART with default config
    uart_reinit(s_uart_port, s_uart_baud, s_uart_tx_pin, s_uart_rx_pin);
    
    ESP_LOGI(TAG, "UART handler task started, listening for 'CF' prefix commands");
    
    while (uart_handler_running) {
        // Check for UART config updates from queue
        if (g_uart_config_queue != NULL) {
            uart_config_data_t uart_cfg;
            if (xQueueReceive(g_uart_config_queue, &uart_cfg, 0) == pdTRUE) {
                ESP_LOGI(TAG, "Received UART config from queue");
                ESP_LOGI(TAG, "Baud: %u, DataBits: %u, StopBits: %u, Parity: %u",
                         uart_cfg.baud_rate, uart_cfg.data_bits, 
                         uart_cfg.stop_bits, uart_cfg.parity);
                
                // For now, only update baud rate (can extend to pins if needed)
                uart_reinit(s_uart_port, uart_cfg.baud_rate, s_uart_tx_pin, s_uart_rx_pin);
            }
        }
        
        // Read UART data
        int len = uart_read_bytes(s_uart_port, data, UART_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        
        if (len > 0) {
            data[len] = '\0';
            
            // Check if command starts with "CF"
            if (len >= 2 && data[0] == 'C' && data[1] == 'F') {
                ESP_LOGI(TAG, "Config command received via UART: %s", (char*)data);
                
                // Skip "CF" prefix and parse actual command
                if (len > 2) {
                    const char *cmd_data = (const char*)(data + 2);
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
                            if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
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
                // Not a config command, handle as normal data
                ESP_LOGI(TAG, "Non-config UART data: %.*s", len, data);
            }
        }
    }
    
    if (s_uart_initialized) {
        uart_driver_delete(s_uart_port);
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
