/*
* USB Handler for ESP32S3
 */
#include "usb_handler.h"

// =============================================================================
// USB SERIAL JTAG Echo Task
// =============================================================================

#define BUF_SIZE (1024)

const char *TAG = "USB_HANDLER";

static TaskHandle_t jtag_task_hdl = NULL;
static TaskHandle_t usb_host_task_hdl = NULL;
static TaskHandle_t class_driver_task_hdl = NULL;
static TaskHandle_t usb_otg_rw_task_hdl = NULL;
static usb_serial_jtag_driver_config_t usb_serial_jtag_config;
static bool close_jtag_task = false;
static bool close_usb_otg_rw_task = false;
static bool usb_host_lib_close = false;

/**
 * @brief JTAG handler task - receives data from USB Serial JTAG and sends to config handler
 * Checks for "CF" prefix to identify configuration commands
 * @param arg Task argument (unused)
 */
static void jtag_task(void *arg) {
    // Configure USB SERIAL JTAG
    usb_serial_jtag_config.rx_buffer_size = BUF_SIZE;
    usb_serial_jtag_config.tx_buffer_size = BUF_SIZE;

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
    ESP_LOGI(TAG, "USB_SERIAL_JTAG init done");

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "no memory for data");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "JTAG handler task started, listening for 'CF' prefix commands");

    while (!close_jtag_task) {
        int len = usb_serial_jtag_read_bytes(data, (BUF_SIZE - 1),
                                             20 / portTICK_PERIOD_MS);

        // Write data back to the USB SERIAL JTAG (echo)
        if (len) {
            usb_serial_jtag_write_bytes((const char *)data, len,
                                        20 / portTICK_PERIOD_MS);
            data[len] = '\0';
            
            // Check if command starts with "CF"
            if (len >= 2 && data[0] == 'C' && data[1] == 'F') {
                ESP_LOGI(TAG, "Config command received via JTAG: %s", (char*)data);
                
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
                            if (xQueueSend(g_config_handler_queue, &cmd, 
                                         pdMS_TO_TICKS(100)) == pdTRUE) {
                                ESP_LOGI(TAG, "Command forwarded to config handler");
                                // Send confirmation back
                                const char *confirm = "\r\nConfig received OK\r\n";
                                usb_serial_jtag_write_bytes(confirm, strlen(confirm), 
                                                           20 / portTICK_PERIOD_MS);
                            } else {
                                ESP_LOGW(TAG, "Failed to send to config handler queue");
                                const char *error = "\r\nConfig queue full\r\n";
                                usb_serial_jtag_write_bytes(error, strlen(error), 
                                                           20 / portTICK_PERIOD_MS);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "Unknown command type in: %s", cmd_data);
                        const char *error = "\r\nUnknown command type\r\n";
                        usb_serial_jtag_write_bytes(error, strlen(error), 
                                                   20 / portTICK_PERIOD_MS);
                    }
                }
            } else {
                // Not a config command, just log it
                ESP_LOGI(TAG, "Non-config JTAG data received");
                ESP_LOG_BUFFER_HEXDUMP("Recv str: ", data, len, ESP_LOG_INFO);
            }
        }
    }

    // Cleanup
    free(data);
    usb_serial_jtag_driver_uninstall();
    ESP_LOGI(TAG, "USB_SERIAL_JTAG deinit done");
    vTaskDelete(NULL);
}

/**
 * @brief Start JTAG handler task
 */
void jtag_task_start(void) {
    close_jtag_task = false;
    BaseType_t task_created;
    
    // Create jtag task pinned to core 0
    task_created = xTaskCreatePinnedToCore(jtag_task, "jtag_handler", 4096, 
                                          NULL, JTAG_TASK_PRIORITY, 
                                          &jtag_task_hdl, 0);
    assert(task_created == pdTRUE);
    ESP_LOGI(TAG, "JTAG handler task created");
}

/**
 * @brief Stop JTAG handler task
 */
void jtag_task_stop(void) {
    close_jtag_task = true;
    ESP_LOGI(TAG, "JTAG handler task stopping");
}