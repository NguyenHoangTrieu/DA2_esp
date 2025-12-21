/**
 * @file pcf8563_rtc.h
 * @brief PCF8563 RTC Driver using I2C Device Support Layer
 */

#ifndef PCF8563_RTC_H
#define PCF8563_RTC_H

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PCF8563 Configuration
#define PCF8563_I2C_ADDR    0x51
#define PCF8563_I2C_FREQ_HZ 400000  // 400kHz (PCF8563 supports up to 400kHz)

/**
 * @brief Initialize PCF8563 RTC (requires i2c_dev_support to be initialized first)
 * @return ESP_OK on success
 */
esp_err_t pcf8563_init(void);

/**
 * @brief Deinitialize PCF8563 RTC
 * @return ESP_OK on success
 */
esp_err_t pcf8563_deinit(void);

/**
 * @brief Read current time from PCF8563
 * @param timeinfo Pointer to tm structure to store time
 * @return ESP_OK on success
 */
esp_err_t pcf8563_read_time(struct tm *timeinfo);

/**
 * @brief Write time to PCF8563
 * @param timeinfo Pointer to tm structure with time to write
 * @return ESP_OK on success
 */
esp_err_t pcf8563_write_time(const struct tm *timeinfo);

/**
 * @brief Check if PCF8563 is running (STOP bit = 0)
 * @param is_running Pointer to store running status
 * @return ESP_OK on success
 */
esp_err_t pcf8563_is_running(bool *is_running);

/**
 * @brief Start PCF8563 clock (clear STOP bit)
 * @return ESP_OK on success
 */
esp_err_t pcf8563_start(void);

/**
 * @brief Stop PCF8563 clock (set STOP bit)
 * @return ESP_OK on success
 */
esp_err_t pcf8563_stop(void);

/**
 * @brief Check if voltage low flag is set
 * @param is_low Pointer to store voltage low status
 * @return ESP_OK on success
 */
esp_err_t pcf8563_check_voltage_low(bool *is_low);

/**
 * @brief Clear voltage low flag
 * @return ESP_OK on success
 */
esp_err_t pcf8563_clear_voltage_low(void);

/**
 * @brief Clear voltage low flag in status register
 * @return ESP_OK on success
 */
esp_err_t pcf8563_clear_voltage_low_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* PCF8563_RTC_H */
