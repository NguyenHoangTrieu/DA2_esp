#ifndef CONFIG_HANDLER_H
#define CONFIG_HANDLER_H

#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "fota_handler.h"

// Command buffer size
#define CONFIG_CMD_MAX_LEN 256
#define CONFIG_QUEUE_SIZE 10

// Command type codes (2-character prefix)
typedef enum {
    CONFIG_TYPE_WIFI = 0,      // "WF" - WiFi configuration
    CONFIG_TYPE_MQTT = 1,      // "MQ" - MQTT configuration  
    CONFIG_TYPE_UART = 2,      // "UR" - UART configuration
    CONFIG_TYPE_USB = 3,       // "US" - USB configuration
    CONFIG_UPDATE_FIRMWARE = 4, // "FW" - Firmware update command
    CONFIG_TYPE_UNKNOWN = 0xFF
} config_type_t;

// WiFi configuration structure
typedef struct {
    char ssid[33];
    char password[65];
} wifi_config_data_t;

// MQTT configuration structure
typedef struct {
    char broker_uri[128];
    char device_token[65];
    uint16_t port;
} mqtt_config_data_t;

// UART configuration structure
typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t stop_bits;
    uint8_t parity;
} uart_config_data_t;

// USB configuration structure
typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t interface_num;
} usb_config_data_t;

// Generic configuration command
typedef struct {
    config_type_t type;
    char raw_data[CONFIG_CMD_MAX_LEN];
    uint16_t data_len;
} config_command_t;

// Queue handles for each subsystem
extern QueueHandle_t g_wifi_config_queue;
extern QueueHandle_t g_mqtt_config_queue;
extern QueueHandle_t g_uart_config_queue;
extern QueueHandle_t g_usb_config_queue;

// Main config handler queue (receives raw commands)
extern QueueHandle_t g_config_handler_queue;

// Function prototypes
void config_handler_task_start(void);
void config_handler_task_stop(void);

// Helper functions
config_type_t config_parse_type(const char *cmd, uint16_t len);
esp_err_t config_parse_wifi(const char *data, uint16_t len, wifi_config_data_t *cfg);
esp_err_t config_parse_mqtt(const char *data, uint16_t len, mqtt_config_data_t *cfg);
esp_err_t config_parse_uart(const char *data, uint16_t len, uart_config_data_t *cfg);

#endif // CONFIG_HANDLER_H
