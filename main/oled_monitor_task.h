/**
 * @file oled_monitor_task.h
 * @brief OLED Monitor Task - Simple display with static icons
 */

#ifndef OLED_MONITOR_TASK_H
#define OLED_MONITOR_TASK_H

#include "esp_err.h"
#include "config_handler.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connection status
extern bool g_oled_wifi_connected;
extern bool g_oled_lte_connected;

// RTC time string
extern char g_oled_rtc_string[32];
extern bool g_oled_rtc_valid;

// Internet connection type
extern config_internet_type_t g_oled_internet_type;

/**
 * @brief Start OLED monitor task
 */
esp_err_t oled_monitor_task_start(void);

/**
 * @brief Stop OLED monitor task
 */
void oled_monitor_task_stop(void);

/**
 * @brief Update WiFi connection status
 */
void oled_monitor_update_wifi(bool connected);

/**
 * @brief Update LTE connection status
 */
void oled_monitor_update_lte(bool connected);

/**
 * @brief Update internet connection type
 */
void oled_monitor_update_internet_type(config_internet_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* OLED_MONITOR_TASK_H */
