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

extern esp_netif_t *g_wifi_netif;
// Config variables (global)
extern char g_wifi_ssid[64];
extern char g_wifi_pass[64];
extern char g_wifi_username[64];
extern wifi_conf_auth_mode_t g_wifi_auth_mode;
// Function prototypes
void wifi_connect_task_start(void);
void wifi_connect_task_stop(void);

#endif // WIFI_CONNECT_H
