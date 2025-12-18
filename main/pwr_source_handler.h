/**
 * @file pwr_source_handler.h
 * @brief Power Source Control Handler via TCA6424A
 */

#ifndef PWR_SOURCE_HANDLER_H
#define PWR_SOURCE_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Power source pin definitions (TCA Port 1)
#define PWR_1V8_PIN 5 // P1_5
#define PWR_3V3_PIN 6 // P1_6
#define PWR_5V0_PIN 7 // P1_7

/**
 * @brief Initialize power source control
 * @note Requires tca_init() to be called first
 * @return ESP_OK on success
 */
esp_err_t pwr_source_init(void);

/**
 * @brief Control 1.8V power rail
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pwr_source_set_1v8(bool enable);

/**
 * @brief Control 3.3V power rail
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pwr_source_set_3v3(bool enable);

/**
 * @brief Control 5.0V power rail
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pwr_source_set_5v0(bool enable);

/**
 * @brief Get 1.8V power rail state
 * @param state Pointer to store state
 * @return ESP_OK on success
 */
esp_err_t pwr_source_get_1v8(bool *state);

/**
 * @brief Get 3.3V power rail state
 * @param state Pointer to store state
 * @return ESP_OK on success
 */
esp_err_t pwr_source_get_3v3(bool *state);

/**
 * @brief Get 5.0V power rail state
 * @param state Pointer to store state
 * @return ESP_OK on success
 */
esp_err_t pwr_source_get_5v0(bool *state);

/**
 * @brief Disable all power rails
 * @return ESP_OK on success
 */
esp_err_t pwr_source_disable_all(void);

#ifdef __cplusplus
}
#endif

#endif /* PWR_SOURCE_HANDLER_H */
