/**
 * @file sh1107_128x128_handler.c
 * @brief SH1107 OLED Display Handler Implementation with built-in font
 */

#include "sh1107_128x128_handler.h"
#include "esp_log.h"
#include "i2c_dev_support.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SH1107";

static i2c_master_dev_handle_t sh1107_handle = NULL;
static uint8_t display_buffer[SH1107_WIDTH * SH1107_HEIGHT / 8];

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
// SH1107 Commands
// ============================================================================
#define SH1107_CMD_DISPLAY_OFF 0xAE
#define SH1107_CMD_DISPLAY_ON 0xAF
#define SH1107_CMD_SET_CONTRAST 0x81
#define SH1107_CMD_SET_NORM_INV 0xA6
#define SH1107_CMD_SET_MUX_RATIO 0xA8
#define SH1107_CMD_SET_DISP_OFFSET 0xD3
#define SH1107_CMD_SET_DISP_START_LINE 0xDC
#define SH1107_CMD_SET_SEGMENT_REMAP 0xA0
#define SH1107_CMD_SET_COM_SCAN_DEC 0xC0
#define SH1107_CMD_SET_DISP_CLK_DIV 0xD5
#define SH1107_CMD_SET_PRECHARGE 0xD9
#define SH1107_CMD_SET_VCOM_DESEL 0xDB
#define SH1107_CMD_SET_CHARGE_PUMP 0xAD
#define SH1107_CMD_SET_MEM_ADDR_MODE 0x20
#define SH1107_CMD_SET_COLUMN_ADDR 0x21
#define SH1107_CMD_SET_PAGE_ADDR 0xB0

static esp_err_t sh1107_write_command(uint8_t cmd) {
  uint8_t data[2] = {0x00, cmd}; // 0x00 = command mode
  return i2c_dev_support_write(sh1107_handle, data, 2, 1000);
}

esp_err_t sh1107_init(void) {
  if (!i2c_dev_support_is_initialized()) {
    ESP_LOGE(TAG, "I2C not initialized. Call i2c_dev_support_init() first");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Initializing SH1107 at address 0x%02X", SH1107_I2C_ADDR);

  esp_err_t ret = i2c_dev_support_add_device(
      SH1107_I2C_ADDR, SH1107_I2C_FREQ_HZ, &sh1107_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SH1107 device: %s", esp_err_to_name(ret));
    return ret;
  }

  // Initialization sequence for SH1107 128x128
  sh1107_write_command(SH1107_CMD_DISPLAY_OFF);

  // Set memory addressing mode to page addressing (0x20)
  sh1107_write_command(SH1107_CMD_SET_MEM_ADDR_MODE);
  sh1107_write_command(0x00); // Page addressing mode

  // Set display offset
  sh1107_write_command(SH1107_CMD_SET_DISP_OFFSET);
  sh1107_write_command(0x00); // Offset by 96 for centering

  // Set display start line
  sh1107_write_command(SH1107_CMD_SET_DISP_START_LINE);
  sh1107_write_command(0x00);

  // Set segment re-map (A0h/A1h)
  sh1107_write_command(SH1107_CMD_SET_SEGMENT_REMAP);

  // Set COM output scan direction
  sh1107_write_command(SH1107_CMD_SET_COM_SCAN_DEC);

  // Set multiplex ratio
  sh1107_write_command(SH1107_CMD_SET_MUX_RATIO);
  sh1107_write_command(0x7F); // 128 MUX (127+1)

  // Set display clock divide ratio/oscillator frequency
  sh1107_write_command(SH1107_CMD_SET_DISP_CLK_DIV);
  sh1107_write_command(0x50);

  // Set contrast control
  sh1107_write_command(SH1107_CMD_SET_CONTRAST);
  sh1107_write_command(0x80); // Medium contrast

  // Set pre-charge period
  sh1107_write_command(SH1107_CMD_SET_PRECHARGE);
  sh1107_write_command(0x22);

  // Set VCOM deselect level
  sh1107_write_command(SH1107_CMD_SET_VCOM_DESEL);
  sh1107_write_command(0x35);

  // Enable charge pump
  sh1107_write_command(SH1107_CMD_SET_CHARGE_PUMP);
  sh1107_write_command(0x8B); // Enable charge pump

  // Set normal display (not inverted)
  sh1107_write_command(SH1107_CMD_SET_NORM_INV);

  // Turn on display
  sh1107_write_command(SH1107_CMD_DISPLAY_ON);

  memset(display_buffer, 0, sizeof(display_buffer));
  sh1107_display();

  ESP_LOGI(TAG, "SH1107 initialized successfully");
  return ESP_OK;
}

esp_err_t sh1107_deinit(void) {
  if (sh1107_handle) {
    esp_err_t ret = i2c_dev_support_remove_device(sh1107_handle);
    sh1107_handle = NULL;
    ESP_LOGI(TAG, "SH1107 deinitialized");
    return ret;
  }
  return ESP_OK;
}

esp_err_t sh1107_clear_display(void) {
  memset(display_buffer, 0, sizeof(display_buffer));
  return ESP_OK;
}

esp_err_t sh1107_display(void) {
  if (!sh1107_handle) {
    return ESP_ERR_INVALID_STATE;
  }

  // Send data in pages (16 pages for 128 rows)
  for (int page = 0; page < 16; page++) {
    // Set page address
    sh1107_write_command(SH1107_CMD_SET_PAGE_ADDR + page);

    // Set column address to 0
    sh1107_write_command(0x00); // Lower nibble
    sh1107_write_command(0x10); // Upper nibble

    // Send page data
    uint8_t chunk[129];
    chunk[0] = 0x40; // Data mode
    memcpy(&chunk[1], &display_buffer[page * 128], 128);
    i2c_dev_support_write(sh1107_handle, chunk, 129, 1000);
  }

  return ESP_OK;
}

esp_err_t sh1107_set_pixel(uint8_t x, uint8_t y, bool color) {
  if (x >= SH1107_WIDTH || y >= SH1107_HEIGHT) {
    return ESP_ERR_INVALID_ARG;
  }

  if (color) {
    display_buffer[x + (y / 8) * SH1107_WIDTH] |= (1 << (y % 8));
  } else {
    display_buffer[x + (y / 8) * SH1107_WIDTH] &= ~(1 << (y % 8));
  }

  return ESP_OK;
}

esp_err_t sh1107_draw_string(uint8_t x, uint8_t y, const char *str,
                             uint8_t size) {
  if (!str) {
    return ESP_ERR_INVALID_ARG;
  }

  if (size < 1)
    size = 1;
  if (size > 3)
    size = 3;

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
    if (x >= SH1107_WIDTH || y >= SH1107_HEIGHT) {
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
              if (px < SH1107_WIDTH && py < SH1107_HEIGHT) {
                int index = px + (py / 8) * SH1107_WIDTH;
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
