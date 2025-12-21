#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"  // Required for Enterprise authentication
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "wifi_scan.h"

// WiFi authentication mode
typedef enum {
    WIFI_AUTH_MODE_PERSONAL = 0,
    WIFI_AUTH_MODE_ENTERPRISE
} wifi_conf_auth_mode_t;

// WiFi configuration context
typedef struct {
    char ssid[64];
    char pass[64];
    char username[64]; // For Enterprise
    wifi_conf_auth_mode_t auth_mode;
} wifi_config_context_t;

extern wifi_config_context_t g_wifi_ctx;

extern esp_netif_t *g_wifi_netif;

// Function prototypes
void wifi_connect_task_start(void);
void wifi_connect_task_stop(void);
bool wifi_is_sntp_synced(void);
uint8_t wifi_get_connection_status(void);
#endif // WIFI_CONNECT_H
