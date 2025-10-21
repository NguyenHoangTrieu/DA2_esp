/*
 * WiFi Scan/SW reset module header for ESP32-P4-WIFI6-DEV-KIT.
 * This header provides all external declarations from the original file.
 * Every function, macro, and comment is preserved for full feature support.
 */

#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

/* Macro defines */
#define DEFAULT_SCAN_LIST_SIZE      20
#define SCAN_INTERVAL_MS            20000  // 20 seconds

#define USE_CHANNEL_BITMAP          1

#if USE_CHANNEL_BITMAP
#define CHANNEL_LIST_SIZE           3
extern uint8_t channel_list[CHANNEL_LIST_SIZE];
void array_2_channel_bitmap(const uint8_t channel_list[], const uint8_t channel_list_size, wifi_scan_config_t *scan_config);
#endif

/* Function declarations (all original functions retained) */
void print_auth_mode(int authmode);
void print_cipher_type(int pairwise_cipher, int group_cipher);
void perform_scan(void);

#endif // WIFI_SCAN_H
