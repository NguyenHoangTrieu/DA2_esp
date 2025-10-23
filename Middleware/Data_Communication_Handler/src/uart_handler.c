#include "config_handler.h"
#include "driver/uart.h"
#include "esp_log.h"

#define UART_PORT_NUM UART_NUM_0
#define UART_BAUD_RATE 115200
#define UART_BUF_SIZE 512

static const char *TAG = "uart_handler";
static bool uart_handler_running = false;

/**
 * @brief UART handler task - receives data from UART and sends to config handler
 * Checks for "CF" prefix to identify configuration commands
 * @param arg Task argument (unused)
 */
static void uart_handler_task(void *arg) {
    uint8_t data[UART_BUF_SIZE];
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    
    ESP_LOGI(TAG, "UART handler task started, listening for 'CF' prefix commands");
    
    while (uart_handler_running) {
        int len = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, 
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
    
    uart_driver_delete(UART_PORT_NUM);
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
