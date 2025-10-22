/*
 * WiFi connection module header for ESP32-P4 board.
 * Declares functions to handle WiFi initialization, UART WiFi credential
 * commands, and hardware reset of attached ESP32-C6 slave. All comments are in
 * English for clarity.
 */

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
#include "mqtt_handler.h"
#include "nvs_flash.h"
#include "wifi_scan.h"

// Function prototypes
void wifi_connect_task_start(void);
void wifi_connect_task_stop(void);

#endif // WIFI_CONNECT_H
