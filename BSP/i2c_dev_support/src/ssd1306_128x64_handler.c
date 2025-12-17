/**
 * @file ssd1306_128x64_handler.c
 * @brief SSD1306 OLED Display Handler Implementation
 */

#include "ssd1306_128x64_handler.h"
#include "i2c_dev_support.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SSD1306";
static i2c_master_dev_handle_t ssd1306_handle = NULL;
static uint8_t display_buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];

// SSD1306 Commands
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_DISPLAY_ON          0xAF
#define SSD1306_CMD_SET_CONTRAST        0x81
#define SSD1306_CMD_SET_ENTIRE_ON       0xA4
#define SSD1306_CMD_SET_NORM_INV        0xA6
#define SSD1306_CMD_SET_MUX_RATIO       0xA8
#define SSD1306_CMD_SET_DISP_OFFSET     0xD3
#define SSD1306_CMD_SET_DISP_START_LINE 0x40
#define SSD1306_CMD_SET_SEGMENT_REMAP   0xA1
#define SSD1306_CMD_SET_COM_SCAN_DEC    0xC8
#define SSD1306_CMD_SET_COM_PINS        0xDA
#define SSD1306_CMD_SET_DISP_CLK_DIV    0xD5
#define SSD1306_CMD_SET_PRECHARGE       0xD9
#define SSD1306_CMD_SET_VCOM_DESEL      0xDB
#define SSD1306_CMD_SET_CHARGE_PUMP     0x8D
#define SSD1306_CMD_SET_MEM_ADDR_MODE   0x20
#define SSD1306_CMD_SET_COLUMN_ADDR     0x21
#define SSD1306_CMD_SET_PAGE_ADDR       0x22

static esp_err_t ssd1306_write_command(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd};  // 0x00 = command mode
    return i2c_dev_support_write(ssd1306_handle, data, 2, 1000);
}

esp_err_t ssd1306_init(void) {
    if (!i2c_dev_support_is_initialized()) {
        ESP_LOGE(TAG, "I2C not initialized. Call i2c_dev_support_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing SSD1306 at address 0x%02X", SSD1306_I2C_ADDR);

    esp_err_t ret = i2c_dev_support_add_device(SSD1306_I2C_ADDR, SSD1306_I2C_FREQ_HZ, &ssd1306_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SSD1306 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialization sequence
    ssd1306_write_command(SSD1306_CMD_DISPLAY_OFF);
    ssd1306_write_command(SSD1306_CMD_SET_DISP_CLK_DIV);
    ssd1306_write_command(0x80);
    ssd1306_write_command(SSD1306_CMD_SET_MUX_RATIO);
    ssd1306_write_command(0x3F);
    ssd1306_write_command(SSD1306_CMD_SET_DISP_OFFSET);
    ssd1306_write_command(0x00);
    ssd1306_write_command(SSD1306_CMD_SET_DISP_START_LINE | 0x00);
    ssd1306_write_command(SSD1306_CMD_SET_CHARGE_PUMP);
    ssd1306_write_command(0x14);
    ssd1306_write_command(SSD1306_CMD_SET_MEM_ADDR_MODE);
    ssd1306_write_command(0x00);
    ssd1306_write_command(SSD1306_CMD_SET_SEGMENT_REMAP);
    ssd1306_write_command(SSD1306_CMD_SET_COM_SCAN_DEC);
    ssd1306_write_command(SSD1306_CMD_SET_COM_PINS);
    ssd1306_write_command(0x12);
    ssd1306_write_command(SSD1306_CMD_SET_CONTRAST);
    ssd1306_write_command(0xCF);
    ssd1306_write_command(SSD1306_CMD_SET_PRECHARGE);
    ssd1306_write_command(0xF1);
    ssd1306_write_command(SSD1306_CMD_SET_VCOM_DESEL);
    ssd1306_write_command(0x40);
    ssd1306_write_command(SSD1306_CMD_SET_ENTIRE_ON);
    ssd1306_write_command(SSD1306_CMD_SET_NORM_INV);
    ssd1306_write_command(SSD1306_CMD_DISPLAY_ON);

    memset(display_buffer, 0, sizeof(display_buffer));
    ssd1306_display();

    ESP_LOGI(TAG, "SSD1306 initialized successfully");
    return ESP_OK;
}

esp_err_t ssd1306_deinit(void) {
    if (ssd1306_handle) {
        esp_err_t ret = i2c_dev_support_remove_device(ssd1306_handle);
        ssd1306_handle = NULL;
        ESP_LOGI(TAG, "SSD1306 deinitialized");
        return ret;
    }
    return ESP_OK;
}

esp_err_t ssd1306_clear_display(void) {
    memset(display_buffer, 0, sizeof(display_buffer));
    return ESP_OK;
}

esp_err_t ssd1306_display(void) {
    if (!ssd1306_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    ssd1306_write_command(SSD1306_CMD_SET_COLUMN_ADDR);
    ssd1306_write_command(0);
    ssd1306_write_command(SSD1306_WIDTH - 1);
    ssd1306_write_command(SSD1306_CMD_SET_PAGE_ADDR);
    ssd1306_write_command(0);
    ssd1306_write_command((SSD1306_HEIGHT / 8) - 1);

    uint8_t *buffer = malloc(sizeof(display_buffer) + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    buffer[0] = 0x40;  // Data mode
    memcpy(buffer + 1, display_buffer, sizeof(display_buffer));
    
    esp_err_t ret = i2c_dev_support_write(ssd1306_handle, buffer, sizeof(display_buffer) + 1, 1000);
    free(buffer);
    
    return ret;
}

esp_err_t ssd1306_set_pixel(uint8_t x, uint8_t y, bool color) {
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (color) {
        display_buffer[x + (y / 8) * SSD1306_WIDTH] |= (1 << (y % 8));
    } else {
        display_buffer[x + (y / 8) * SSD1306_WIDTH] &= ~(1 << (y % 8));
    }

    return ESP_OK;
}

esp_err_t ssd1306_draw_string(uint8_t x, uint8_t y, const char *str) {
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Draw string at (%d,%d): %s", x, y, str);
    return ESP_OK;
}
