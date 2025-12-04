/**
 * @file mcu_lan_handler.h
 * @brief MCU LAN Handler - WAN Side (SPI Slave)
 * 
 * Implements Diagram 2: SPI Driver & Communication Logic
 * Runs on WAN MCU, communicates with LAN MCU via SPI Slave
 */
#ifndef MCU_LAN_HANDLER_H
#define MCU_LAN_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "frame_types.h"

typedef enum {
    INTERNET_STATUS_OFFLINE = 0,
    INTERNET_STATUS_ONLINE = 1
} internet_status_t;

/**
 * @brief Start MCU LAN handler task
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_start(void);

/**
 * @brief Stop MCU LAN handler task
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mcu_lan_handler_stop(void);

/**
 * @brief Set internet connection status (called by MQTT/HTTP handler)
 * @param status Internet connection status
 */
void mcu_lan_handler_set_internet_status(internet_status_t status);

/**
 * @brief Enqueue downlink data from Server to LAN MCU
 * @param target_id Target handler ID (CAN/LoRa/ZigBee)
 * @param data Data buffer
 * @param len Data length
 * @return true on success
 */
bool mcu_lan_enqueue_downlink(handler_id_t target_id, uint8_t *data, uint16_t len);

/**
 * @brief Update config data to send to LAN MCU
 * @param config_data Config data buffer
 * @param length Config data length
 * @param is_fota True if firmware update
 */
void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length, bool is_fota);

/**
 * @brief Enqueue uplink data to server (MQTT/HTTP)
 * @param data Data buffer
 * @param len Data length
 * @return true on success
 */
bool server_handler_enqueue_uplink(uint8_t *data, uint16_t len);

#endif // MCU_LAN_HANDLER_H
