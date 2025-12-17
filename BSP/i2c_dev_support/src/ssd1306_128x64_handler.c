/**
 * @file ssd1306_128x64_handler.c
 * @brief SSD1306 OLED Display Handler Implementation with built-in font
 */

#include "ssd1306_128x64_handler.h"
#include "i2c_dev_support.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SSD1306";

static i2c_master_dev_handle_t ssd1306_handle = NULL;
static uint8_t display_buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];

// ============================================================================
// Simple 5x7 Font (ASCII 32-127)
// ============================================================================
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // (space)
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x41, 0x22, 0x14, 0x08, 0x00}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x01, 0x01}, // F
    {0x3E, 0x41, 0x41, 0x51, 0x32}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x00, 0x7F, 0x41, 0x41}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x41, 0x41, 0x7F, 0x00, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x08, 0x14, 0x54, 0x54, 0x3C}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x00, 0x7F, 0x10, 0x28, 0x44}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x08, 0x04, 0x08, 0x10, 0x08}, // ~
};

// ============================================================================
// SSD1306 Commands
// ============================================================================
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
    uint8_t data[2] = {0x00, cmd}; // 0x00 = command mode
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

    // Send data in chunks (128 bytes per page)
    for (int page = 0; page < 8; page++) {
        uint8_t chunk[129];
        chunk[0] = 0x40; // Data mode
        memcpy(&chunk[1], &display_buffer[page * 128], 128);
        i2c_dev_support_write(ssd1306_handle, chunk, 129, 1000);
    }

    return ESP_OK;
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

esp_err_t ssd1306_draw_string(uint8_t x, uint8_t y, const char *str, uint8_t size) {
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size < 1) size = 1;
    if (size > 3) size = 3;

    uint8_t orig_x = x;

    while (*str) {
        char c = *str;

        // Handle line break
        if (c == '\n') {
            y += 8 * size;
            x = orig_x;
            str++;
            continue;
        }

        // Check bounds
        if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
            break;
        }

        // Get font data
        const uint8_t *glyph;
        if (c >= 32 && c <= 126) {
            glyph = font5x7[c - 32];
        } else {
            glyph = font5x7[0]; // Space for unknown chars
        }

        // Draw character
        for (int i = 0; i < 5; i++) {
            uint8_t line = glyph[i];
            for (int j = 0; j < 8; j++) {
                if (line & (1 << j)) {
                    // Draw pixel at (x+i*size, y+j*size)
                    for (int dx = 0; dx < size; dx++) {
                        for (int dy = 0; dy < size; dy++) {
                            int px = x + i * size + dx;
                            int py = y + j * size + dy;

                            if (px < SSD1306_WIDTH && py < SSD1306_HEIGHT) {
                                int index = px + (py / 8) * SSD1306_WIDTH;
                                display_buffer[index] |= (1 << (py % 8));
                            }
                        }
                    }
                }
            }
        }

        x += 6 * size; // 5 pixels + 1 space
        str++;
    }

    return ESP_OK;
}
