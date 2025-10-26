#ifndef LTE_CONNECT_H
#define LTE_CONNECT_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Start LTE connection task
 * 
 * This will:
 * 1. Initialize LTE Handler with default/stored config
 * 2. Attempt initial connection
 * 3. Start task to listen for config updates from queue
 */
void lte_connect_task_start(void);

/**
 * @brief Stop LTE connection task
 * 
 * Gracefully disconnects and deinitializes LTE Handler
 */
void lte_connect_task_stop(void);

/**
 * @brief Check if LTE is currently connected
 * 
 * @return true if connected, false otherwise
 */
bool lte_is_connected(void);

/**
 * @brief Get current signal strength
 * 
 * @param rssi Pointer to store RSSI value
 * @param ber Pointer to store BER value
 * @return ESP_OK on success
 */
esp_err_t lte_get_signal_strength(uint32_t *rssi, uint32_t *ber);

#endif /* LTE_CONNECT_H */
