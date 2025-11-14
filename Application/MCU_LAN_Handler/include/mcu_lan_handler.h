/**
 * @file mcu_lan_handler.h
 * @brief MCU LAN Communication Handler
 * 
 * Handles sending commands from WAN MCU to LAN MCU via SPI Master
 * Uses config_handler's queue and structures
 */

#ifndef MCU_LAN_HANDLER_H
#define MCU_LAN_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start MCU LAN handler
 * 
 * Automatically initializes on first call (init happens only once in lifetime)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_start(void);

/**
 * @brief Stop MCU LAN handler
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_stop(void);

/**
 * @brief Start the INIT_OK check timer
 * 
 */
void mcu_lan_start_timer(void);

#endif // MCU_LAN_HANDLER_H
