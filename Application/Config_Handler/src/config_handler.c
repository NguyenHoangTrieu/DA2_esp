#include "config_handler.h"
#include "esp_log.h"
#include <ctype.h>

static const char *TAG = "config_handler";

// Queue handles
QueueHandle_t g_wifi_config_queue = NULL;
QueueHandle_t g_mqtt_config_queue = NULL;
QueueHandle_t g_uart_config_queue = NULL;
QueueHandle_t g_usb_config_queue = NULL;
QueueHandle_t g_config_handler_queue = NULL;

static bool config_handler_running = false;
static TaskHandle_t config_handler_task_handle = NULL;

/**
 * @brief Parse command type from 2-character prefix
 * @param cmd Command string
 * @param len Command length
 * @return config_type_t Command type enum
 */
config_type_t config_parse_type(const char *cmd, uint16_t len) {
    if (len < 2) {
        return CONFIG_TYPE_UNKNOWN;
    }
    
    // Check first 2 characters
    if (cmd[0] == 'W' && cmd[1] == 'F') {
        return CONFIG_TYPE_WIFI;
    } else if (cmd[0] == 'M' && cmd[1] == 'Q') {
        return CONFIG_TYPE_MQTT;
    } else if (cmd[0] == 'U' && cmd[1] == 'R') {
        return CONFIG_TYPE_UART;
    } else if (cmd[0] == 'U' && cmd[1] == 'S') {
        return CONFIG_TYPE_USB;
    }
    
    return CONFIG_TYPE_UNKNOWN;
}

/**
 * @brief Parse WiFi configuration from command string
 * Format: "WF:SSID:PASSWORD"
 * Example: "WF:MyWiFi:MyPassword123"
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output WiFi config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t config_parse_wifi(const char *data, uint16_t len, wifi_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }
    
    // Find first colon after "WF"
    const char *first_colon = strchr(data + 2, ':');
    if (!first_colon) {
        ESP_LOGE(TAG, "WiFi config format error: missing first ':'");
        return ESP_FAIL;
    }
    
    // Find second colon
    const char *second_colon = strchr(first_colon + 1, ':');
    if (!second_colon) {
        ESP_LOGE(TAG, "WiFi config format error: missing second ':'");
        return ESP_FAIL;
    }
    
    // Extract SSID
    int ssid_len = second_colon - first_colon - 1;
    if (ssid_len <= 0 || ssid_len >= 33) {
        ESP_LOGE(TAG, "WiFi SSID length invalid: %d", ssid_len);
        return ESP_FAIL;
    }
    
    memset(cfg->ssid, 0, sizeof(cfg->ssid));
    memcpy(cfg->ssid, first_colon + 1, ssid_len);
    
    // Extract password
    int pass_len = len - (second_colon - data) - 1;
    if (pass_len < 0 || pass_len >= 65) {
        ESP_LOGE(TAG, "WiFi password length invalid: %d", pass_len);
        return ESP_FAIL;
    }
    
    memset(cfg->password, 0, sizeof(cfg->password));
    if (pass_len > 0) {
        memcpy(cfg->password, second_colon + 1, pass_len);
    }
    
    ESP_LOGI(TAG, "Parsed WiFi config - SSID: '%s', Pass: '%s'", cfg->ssid, cfg->password);
    return ESP_OK;
}

/**
 * @brief Parse MQTT configuration from command string
 * Format: "MQ:BROKER_URI:PORT:TOKEN"
 * Example: "MQ:mqtt://demo.thingsboard.io:1883:myDeviceToken123"
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output MQTT config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t config_parse_mqtt(const char *data, uint16_t len, mqtt_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }
    
    // Parse format: "MQ:broker_uri:port:token"
    const char *ptr = data + 3; // Skip "MQ:"
    const char *end = data + len;
    
    // Parse broker URI
    const char *first_colon = strchr(ptr, ':');
    if (!first_colon || first_colon >= end) {
        ESP_LOGE(TAG, "MQTT config: missing broker URI separator");
        return ESP_FAIL;
    }
    
    int uri_len = first_colon - ptr;
    if (uri_len <= 0 || uri_len >= sizeof(cfg->broker_uri)) {
        ESP_LOGE(TAG, "MQTT broker URI length invalid: %d", uri_len);
        return ESP_FAIL;
    }
    
    memset(cfg->broker_uri, 0, sizeof(cfg->broker_uri));
    memcpy(cfg->broker_uri, ptr, uri_len);
    
    // Parse port
    ptr = first_colon + 1;
    const char *second_colon = strchr(ptr, ':');
    if (!second_colon || second_colon >= end) {
        ESP_LOGE(TAG, "MQTT config: missing port separator");
        return ESP_FAIL;
    }
    
    cfg->port = atoi(ptr);
    if (cfg->port == 0) {
        ESP_LOGE(TAG, "MQTT port invalid");
        return ESP_FAIL;
    }
    
    // Parse token
    ptr = second_colon + 1;
    int token_len = end - ptr;
    if (token_len <= 0 || token_len >= sizeof(cfg->device_token)) {
        ESP_LOGE(TAG, "MQTT token length invalid: %d", token_len);
        return ESP_FAIL;
    }
    
    memset(cfg->device_token, 0, sizeof(cfg->device_token));
    memcpy(cfg->device_token, ptr, token_len);
    
    ESP_LOGI(TAG, "Parsed MQTT config - URI: '%s', Port: %d, Token: '%s'", 
             cfg->broker_uri, cfg->port, cfg->device_token);
    return ESP_OK;
}

/**
 * @brief Parse UART configuration from command string
 * Format: "UR:BAUDRATE:DATABITS:STOPBITS:PARITY"
 * Example: "UR:115200:8:1:0"
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output UART config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t config_parse_uart(const char *data, uint16_t len, uart_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }
    
    // Simple parsing: "UR:baudrate:databits:stopbits:parity"
    int parsed = sscanf(data, "UR:%u:%hhu:%hhu:%hhu", 
                       &cfg->baud_rate, &cfg->data_bits, 
                       &cfg->stop_bits, &cfg->parity);
    
    if (parsed != 4) {
        ESP_LOGE(TAG, "UART config parse error (parsed %d/4 fields)", parsed);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Parsed UART config - Baud: %u, Data: %u, Stop: %u, Parity: %u",
             cfg->baud_rate, cfg->data_bits, cfg->stop_bits, cfg->parity);
    return ESP_OK;
}

/**
 * @brief Config handler task - receives raw commands and routes to specific queues
 * @param arg Task argument (unused)
 */
static void config_handler_task(void *arg) {
    config_command_t cmd;
    
    ESP_LOGI(TAG, "Config handler task started");
    
    while (config_handler_running) {
        // Wait for command from gateway
        if (xQueueReceive(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Received config command, type: %d, len: %d", cmd.type, cmd.data_len);
            
            // Route to appropriate handler based on type
            switch (cmd.type) {
                case CONFIG_TYPE_WIFI: {
                    wifi_config_data_t wifi_cfg;
                    if (config_parse_wifi(cmd.raw_data, cmd.data_len, &wifi_cfg) == ESP_OK) {
                        if (g_wifi_config_queue) {
                            xQueueSend(g_wifi_config_queue, &wifi_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "WiFi config sent to WiFi task");
                        } else {
                            ESP_LOGW(TAG, "WiFi queue not initialized");
                        }
                    }
                    break;
                }
                
                case CONFIG_TYPE_MQTT: {
                    mqtt_config_data_t mqtt_cfg;
                    if (config_parse_mqtt(cmd.raw_data, cmd.data_len, &mqtt_cfg) == ESP_OK) {
                        if (g_mqtt_config_queue) {
                            xQueueSend(g_mqtt_config_queue, &mqtt_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "MQTT config sent to MQTT task");
                        } else {
                            ESP_LOGW(TAG, "MQTT queue not initialized");
                        }
                    }
                    break;
                }
                
                case CONFIG_TYPE_UART: {
                    uart_config_data_t uart_cfg;
                    if (config_parse_uart(cmd.raw_data, cmd.data_len, &uart_cfg) == ESP_OK) {
                        if (g_uart_config_queue) {
                            xQueueSend(g_uart_config_queue, &uart_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "UART config sent to UART task");
                        } else {
                            ESP_LOGW(TAG, "UART queue not initialized");
                        }
                    }
                    break;
                }
                
                case CONFIG_TYPE_USB: {
                    // USB config parsing can be added similarly
                    ESP_LOGI(TAG, "USB config received (not implemented yet)");
                    break;
                }
                
                default:
                    ESP_LOGW(TAG, "Unknown config type: %d", cmd.type);
                    break;
            }
        }
    }
    
    ESP_LOGI(TAG, "Config handler task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Start config handler task and initialize queues
 */
void config_handler_task_start(void) {
    if (config_handler_running) {
        ESP_LOGW(TAG, "Config handler already running");
        return;
    }
    
    // Create queues if not exists
    if (!g_config_handler_queue) {
        g_config_handler_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(config_command_t));
    }
    
    if (!g_wifi_config_queue) {
        g_wifi_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(wifi_config_data_t));
    }
    
    if (!g_mqtt_config_queue) {
        g_mqtt_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(mqtt_config_data_t));
    }
    
    if (!g_uart_config_queue) {
        g_uart_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(uart_config_data_t));
    }
    
    if (!g_usb_config_queue) {
        g_usb_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(usb_config_data_t));
    }
    
    config_handler_running = true;
    xTaskCreate(config_handler_task, "config_handler", 4096, NULL, 5, &config_handler_task_handle);
    ESP_LOGI(TAG, "Config handler task created");
}

/**
 * @brief Stop config handler task
 */
void config_handler_task_stop(void) {
    config_handler_running = false;
    ESP_LOGI(TAG, "Config handler task stopping");
}
