/**
 * @file sh1107_128x128_handler.h
 * @brief SH1107 128x128 OLED Display Handler using I2C Device Support
 */

#ifndef SH1107_128X128_HANDLER_H
#define SH1107_128X128_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SH1107 Configuration
#define SH1107_I2C_ADDR 0x3C
#define SH1107_I2C_FREQ_HZ 400000 // 400kHz
#define SH1107_WIDTH 128
#define SH1107_HEIGHT 128

/**
 * @brief Initialize SH1107 display (requires i2c_dev_support to be initialized
 * first)
 * @return ESP_OK on success
 */
esp_err_t sh1107_init(void);

/**
 * @brief Deinitialize SH1107 display
 * @return ESP_OK on success
 */
esp_err_t sh1107_deinit(void);

/**
 * @brief Clear display buffer
 * @return ESP_OK on success
 */
esp_err_t sh1107_clear_display(void);

/**
 * @brief Update physical display with buffer content
 * @return ESP_OK on success
 */
esp_err_t sh1107_display(void);

/**
 * @brief Set pixel at position
 * @param x X coordinate (0-127)
 * @param y Y coordinate (0-127)
 * @param color 1 for white, 0 for black
 * @return ESP_OK on success
 */
esp_err_t sh1107_set_pixel(uint8_t x, uint8_t y, bool color);

/**
 * @brief Draw string at position with specified size
 * @param x X coordinate
 * @param y Y coordinate
 * @param str String to draw
 * @param size Font size (1=small, 2=medium, 3=large)
 * @return ESP_OK on success
 */
esp_err_t sh1107_draw_string(uint8_t x, uint8_t y, const char *str,
                             uint8_t size);

#ifdef __cplusplus
}
#endif

#endif /* SH1107_128X128_HANDLER_H */
