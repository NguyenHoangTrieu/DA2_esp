#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "wifi_scan.h"

// Function prototypes
extern esp_netif_t *g_wifi_netif;
void wifi_connect_task_start(void);
void wifi_connect_task_stop(void);

#endif // WIFI_CONNECT_H
