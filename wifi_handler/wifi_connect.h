/*
 * WiFi connection module header for ESP32-P4 board.
 * Declares functions to handle WiFi initialization, UART WiFi credential commands,
 * and hardware reset of attached ESP32-C6 slave.
 * All comments are in English for clarity.
 */

#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "driver/uart.h"

// Main WiFi initialization function. Call once at boot.
// Allows passing custom SSID/PASSWORD for initial connect.
void wifi_init_sta(const char *custom_ssid, const char *custom_pass);

// Perform a software reset of the attached ESP32-C6 slave via GPIO54.
// Typically called on hardware boot/reset.
void reset_c6_slave(void);

void wifi_connect_task_start(void);

#endif // WIFI_CONNECT_H
