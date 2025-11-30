/**
 * @file mcu_lan_handler.h
 * @brief MCU LAN Communication Handler (SPI Slave - WAN MCU side)
 * 
 * Runs on WAN MCU, communicates with LAN MCU (master) via SPI Slave.
 */

#ifndef MCU_LAN_HANDLER_H
#define MCU_LAN_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Internet connection status
 */
typedef enum {
    INTERNET_STATUS_OFFLINE = 0,
    INTERNET_STATUS_ONLINE = 1
} internet_status_t;

/**
 * @brief Start MCU LAN handler task
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_start(void);

/**
 * @brief Stop MCU LAN handler task
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_stop(void);

/**
 * @brief Update configuration data to send to LAN MCU
 * 
 * Called by config handler to update config cache.
 * 
 * @param config_data Pointer to config data buffer (with prefix: CFFW, CFCA, etc.)
 * @param length Length of config data
 * @param is_fota True if this is FOTA config
 */
void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length, bool is_fota);

/**
 * @brief Set internet connection status
 * 
 * Called by network/MQTT handler to update internet status.
 * 
 * @param status Internet connection status
 */
void mcu_lan_handler_set_internet_status(internet_status_t status);

#endif // MCU_LAN_HANDLER_H
