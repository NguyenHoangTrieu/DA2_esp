#ifndef CONFIG_HANDLER_H
#define CONFIG_HANDLER_H

#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "fota_handler.h"
#include "ppp_server.h"
#include "mcu_lan_handler.h"
#include "mqtt_handler.h"
#include "wifi_connect.h"
#include "lte_connect.h"
#include "lte_handler.h"
#include "nvs_flash.h"
#include "nvs.h"

// Command buffer size - Increased to support large JSON configs (up to 4KB)
// Must match MAX_CONFIG_LENGTH in uart_handler.c
#define CONFIG_CMD_MAX_LEN 8192
#define CONFIG_QUEUE_SIZE 20

// Command type codes (2-character prefix)
typedef enum {
    CONFIG_TYPE_WIFI = 0,      // "WF" - WiFi configuration
    CONFIG_TYPE_MQTT = 1,      // "MQ" - MQTT configuration
    CONFIG_TYPE_LTE = 2,       // "LT" - LTE configuration
    CONFIG_TYPE_INTERNET = 3,   // "IN" - Internet configuration
    CONFIG_UPDATE_FIRMWARE = 4, // "FW" - Firmware update command
    CONFIG_TYPE_MCU_LAN = 5,    // "ML" - MCU LAN configuration
    CONFIG_TYPE_SERVER = 6,     // "SV" - Server configuration
    CONFIG_TYPE_UNKNOWN = 0xFF
} config_type_t;

typedef enum {
    CONFIG_INTERNET_WIFI = 0,
    CONFIG_INTERNET_LTE = 1,
    CONFIG_INTERNET_ETHERNET = 2,
    CONFIG_INTERNET_NBIOT = 3,
    CONFIG_INTERNET_COUNT = 4,
    CONFIG_INTERNET_TYPE_UNKNOWN = 0xFF
} config_internet_type_t;

typedef enum {
    CONFIG_SERVERTYPE_MQTT = 0,
    CONFIG_SERVERTYPE_COAP = 1,
    CONFIG_SERVERTYPE_HTTP = 2,
    CONFIG_SERVERTYPE_COUNT = 3,
    CONFIG_SERVERTYPE_UNKNOWN = 0xFF
} config_server_type_t;
// WiFi configuration structure
typedef struct {
    char username[64];
    char ssid[64];
    char password[64];
    wifi_conf_auth_mode_t auth_mode;
} wifi_config_data_t;

// LTE configuration structure
typedef struct {
    char apn[64];           // Access Point Name
    char username[32];      // PPP username (optional)
    char password[32];      // PPP password (optional)
    lte_handler_comm_type_t comm_type;
    bool auto_reconnect;
    uint32_t reconnect_timeout_ms;
    uint32_t max_reconnect_attempts;
} lte_config_data_t;

// MQTT configuration structure
typedef struct {
    char broker_uri[128];
    char device_token[65];
    char subscribe_topic[128];
    char attribute_topic[128];
    char publish_topic[128];
} mqtt_config_data_t;

//MCU LAN Communication configuration structure
// Increased buffer size to match CONFIG_CMD_MAX_LEN for large JSON configs
typedef struct {
    char command[CONFIG_CMD_MAX_LEN];
    int  length;
} mcu_lan_config_data_t;

// Generic configuration command
typedef struct {
    config_type_t type;
    char raw_data[CONFIG_CMD_MAX_LEN];
    uint16_t data_len;
    command_source_t source;  // CMD_SOURCE_UART / CMD_SOURCE_USB / CMD_SOURCE_MQTT
} config_command_t;

// Queue handles for each subsystem
extern QueueHandle_t g_wifi_config_queue;
extern QueueHandle_t g_lte_config_queue;
extern QueueHandle_t g_mqtt_config_queue;
extern QueueHandle_t g_mcu_lan_config_queue;

extern bool is_internet_connected;

// Main config handler queue (receives raw commands)
extern QueueHandle_t g_config_handler_queue;
extern config_internet_type_t g_internet_type;
extern config_server_type_t g_server_type;
// Function prototypes
void config_handler_task_start(void);
void config_handler_task_stop(void);

// Helper functions
config_type_t config_parse_type(const char *cmd, uint16_t len);

// NVS save/load functions
esp_err_t save_internet_config_to_nvs(void);
esp_err_t save_server_config_to_nvs(void);
esp_err_t save_mqtt_config_to_nvs(void);
esp_err_t save_lte_config_to_nvs(void);
esp_err_t save_wifi_config_to_nvs(void);

esp_err_t erase_all_configs_from_nvs(void);
esp_err_t config_init(void);

// Thread-safe config access functions
esp_err_t config_get_wifi_safe(wifi_config_context_t *out_cfg);
esp_err_t config_update_wifi_safe(const wifi_config_data_t *new_cfg);
esp_err_t config_get_lte_safe(lte_config_context_t *out_cfg);
esp_err_t config_update_lte_safe(const lte_config_data_t *new_cfg);
esp_err_t config_get_mqtt_safe(mqtt_config_context_t *out_cfg);
esp_err_t config_update_mqtt_safe(const mqtt_config_data_t *new_cfg);

#endif // CONFIG_HANDLER_H
