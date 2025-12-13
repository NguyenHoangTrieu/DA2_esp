/**
 * @file ds1307_rtc.h
 * @brief DS1307 RTC Driver using ESP-IDF 6.0 I2C Master API
 */

#ifndef DS1307_RTC_H
#define DS1307_RTC_H

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// DS1307 Configuration
#define DS1307_I2C_ADDR 0x68
#define DS1307_I2C_PORT I2C_NUM_0
#define DS1307_I2C_SDA_PIN 21
#define DS1307_I2C_SCL_PIN 22
#define DS1307_I2C_FREQ_HZ 100000 // 100kHz

/**
 * @brief Initialize DS1307 RTC with I2C master
 * @return ESP_OK on success
 */
esp_err_t ds1307_init(void);

/**
 * @brief Deinitialize DS1307 RTC
 * @return ESP_OK on success
 */
esp_err_t ds1307_deinit(void);

/**
 * @brief Read current time from DS1307
 * @param timeinfo Pointer to tm structure to store time
 * @return ESP_OK on success
 */
esp_err_t ds1307_read_time(struct tm *timeinfo);

/**
 * @brief Write time to DS1307
 * @param timeinfo Pointer to tm structure with time to write
 * @return ESP_OK on success
 */
esp_err_t ds1307_write_time(const struct tm *timeinfo);

/**
 * @brief Check if DS1307 is running (CH bit = 0)
 * @param is_running Pointer to store running status
 * @return ESP_OK on success
 */
esp_err_t ds1307_is_running(bool *is_running);

/**
 * @brief Start DS1307 clock (clear CH bit)
 * @return ESP_OK on success
 */
esp_err_t ds1307_start(void);

/**
 * @brief Stop DS1307 clock (set CH bit)
 * @return ESP_OK on success
 */
esp_err_t ds1307_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DS1307_RTC_H */
