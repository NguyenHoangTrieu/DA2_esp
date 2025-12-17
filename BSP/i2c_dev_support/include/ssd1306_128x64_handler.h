/**
 * @file ssd1306_128x64_handler.h
 * @brief SSD1306 128x64 OLED Display Handler using I2C Device Support
 */

#ifndef SSD1306_128X64_HANDLER_H
#define SSD1306_128X64_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SSD1306 Configuration
#define SSD1306_I2C_ADDR    0x3C
#define SSD1306_I2C_FREQ_HZ 400000 // 400kHz

#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64

/**
 * @brief Initialize SSD1306 display (requires i2c_dev_support to be initialized first)
 * @return ESP_OK on success
 */
esp_err_t ssd1306_init(void);

/**
 * @brief Deinitialize SSD1306 display
 * @return ESP_OK on success
 */
esp_err_t ssd1306_deinit(void);

/**
 * @brief Clear display buffer
 * @return ESP_OK on success
 */
esp_err_t ssd1306_clear_display(void);

/**
 * @brief Update physical display with buffer content
 * @return ESP_OK on success
 */
esp_err_t ssd1306_display(void);

/**
 * @brief Set pixel at position
 * @param x X coordinate (0-127)
 * @param y Y coordinate (0-63)
 * @param color 1 for white, 0 for black
 * @return ESP_OK on success
 */
esp_err_t ssd1306_set_pixel(uint8_t x, uint8_t y, bool color);

/**
 * @brief Draw string at position with specified size
 * @param x X coordinate
 * @param y Y coordinate
 * @param str String to draw
 * @param size Font size (1=small, 2=medium, 3=large)
 * @return ESP_OK on success
 */
esp_err_t ssd1306_draw_string(uint8_t x, uint8_t y, const char *str, uint8_t size);

#ifdef __cplusplus
}
#endif

#endif /* SSD1306_128X64_HANDLER_H */
