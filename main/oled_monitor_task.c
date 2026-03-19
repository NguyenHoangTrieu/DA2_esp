/**
 * @file oled_monitor_task.c
 * @brief OLED Monitor - Premium Design with Animations (128x128)
 */

#include "oled_monitor_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcf8563_rtc.h"
#include "sh1107_128x128_handler.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "OLED_MON";

// Configuration
#define OLED_TASK_STACK_SIZE 4096
#define OLED_TASK_PRIORITY 3
#define OLED_UPDATE_INTERVAL_MS 1000

// Global Variables
bool g_oled_wifi_connected = false;
bool g_oled_lte_connected = false;
bool g_oled_eth_connected = false;
char g_oled_rtc_string[32] = "00/00/0000 00:00:00";
bool g_oled_rtc_valid = false;
config_internet_type_t g_oled_internet_type = CONFIG_INTERNET_WIFI;

// Private Variables
static TaskHandle_t g_oled_task_handle = NULL;
static SemaphoreHandle_t g_oled_mutex = NULL;
static bool g_oled_task_running = false;
static uint8_t blink_state = 0;

// WiFi Icon - Standard 3-arc signal (16x14 pixels)
static const uint8_t wifi_icon[] = {
    0b00001111, 0b11110000, 0b00000000, // Row 0
    0b00110000, 0b00001100, 0b00000000, // Row 1
    0b01000000, 0b00000010, 0b00000000, // Row 2
    0b00000000, 0b00000000, 0b00000000, // Row 3
    0b00000111, 0b11100000, 0b00000000, // Row 4
    0b00011000, 0b00011000, 0b00000000, // Row 5
    0b00100000, 0b00000100, 0b00000000, // Row 6
    0b00000000, 0b00000000, 0b00000000, // Row 7
    0b00000011, 0b11000000, 0b00000000, // Row 8
    0b00000100, 0b00100000, 0b00000000, // Row 9
    0b00000000, 0b00000000, 0b00000000, // Row 10
    0b00000001, 0b10000000, 0b00000000, // Row 11
    0b00000011, 0b11000000, 0b00000000, // Row 12
    0b00000001, 0b10000000, 0b00000000, // Row 13
};

// LTE Icon - Modern 4G with bars (20x14 pixels) - FIXED FORMAT
static const uint8_t lte_icon[] = {
    0b00000000, 0b00000000, 0b00000000, // Row 0
    0b00000000, 0b00000000, 0b00000000, // Row 1
    0b00000000, 0b00000000, 0b00000000, // Row 2
    0b00000000, 0b00000111, 0b00000000, // Row 3
    0b00000000, 0b00000111, 0b00000000, // Row 4
    0b00000000, 0b01110111, 0b00000000, // Row 5
    0b00000000, 0b01110111, 0b00000000, // Row 6
    0b00000111, 0b01110111, 0b00000000, // Row 7
    0b00000111, 0b01110111, 0b00000000, // Row 8
    0b01110111, 0b01110111, 0b00000000, // Row 9
    0b01110111, 0b01110111, 0b00000000, // Row 10
    0b01110111, 0b01110111, 0b00000000, // Row 11
};

// Status dot (for animation)
static const uint8_t status_dot_filled[] = {
    0b00111100, 0b01111110, 0b11111111, 0b11111111,
    0b11111111, 0b11111111, 0b01111110, 0b00111100,
};

static const uint8_t status_dot_empty[] = {
    0b00111100, 0b01000010, 0b10000001, 0b10000001,
    0b10000001, 0b10000001, 0b01000010, 0b00111100,
};

// Private Function Prototypes

static void get_current_time_string(char *buffer, int *hour, int *min) {
    if (!buffer || !hour || !min) {
        return;
    }
    
    // Always use system time (synced via SNTP)
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Extract and validate components to prevent buffer overflow
    int year = timeinfo.tm_year + 1900;
    int month = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;
    int sec = timeinfo.tm_sec;
    
    // Clamp values to valid ranges
    if (year < 2000 || year > 2099) year = 2025;
    if (month < 1 || month > 12) month = 1;
    if (day < 1 || day > 31) day = 1;
    if (timeinfo.tm_hour < 0 || timeinfo.tm_hour > 23) {
        *hour = 0;
    } else {
        *hour = timeinfo.tm_hour;
    }
    if (timeinfo.tm_min < 0 || timeinfo.tm_min > 59) {
        *min = 0;
    } else {
        *min = timeinfo.tm_min;
    }
    if (sec < 0 || sec > 59) sec = 0;
    
    // Format time string - guaranteed to be exactly 19 chars + null terminator
    // Format: "DD/MM/YYYY HH:MM:SS" = 19 chars
    int written = snprintf(buffer, 32, "%02d/%02d/%04d %02d:%02d:%02d",
                           day, month, year, *hour, *min, sec);
    
    // Ensure null termination (defensive programming)
    if (written >= 32) {
        buffer[31] = '\0';
    }
}

static void draw_border(void) {
  // Top and bottom lines
  for (int x = 0; x < 128; x++) {
    sh1107_set_pixel(x, 0, true);
    sh1107_set_pixel(x, 127, true);
  }

  // Left and right lines
  for (int y = 0; y < 128; y++) {
    sh1107_set_pixel(0, y, true);
    sh1107_set_pixel(127, y, true);
  }
}

static void draw_status_dot(uint8_t x, uint8_t y, bool filled) {
  const uint8_t *dot = filled ? status_dot_filled : status_dot_empty;
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (dot[i] & (0x80 >> j)) {
        sh1107_set_pixel(x + j, y + i, true);
      }
    }
  }
}

static void draw_wifi_icon_display(uint8_t x, uint8_t y) {
  for (int row = 0; row < 14; row++) {
    uint32_t bits = ((uint32_t)wifi_icon[row * 3] << 16) |
                    ((uint32_t)wifi_icon[row * 3 + 1] << 8) |
                    wifi_icon[row * 3 + 2];
    for (int col = 0; col < 16; col++) {
      if (bits & (0x800000 >> col)) {
        sh1107_set_pixel(x + col, y + row, true);
      }
    }
  }
}

static void draw_lte_icon_display(uint8_t x, uint8_t y) {
  for (int row = 0; row < 12; row++) {
    uint32_t bits = ((uint32_t)lte_icon[row * 3] << 16) |
                    ((uint32_t)lte_icon[row * 3 + 1] << 8) |
                    lte_icon[row * 3 + 2];
    for (int col = 0; col < 20; col++) {
      if (bits & (0x800000 >> col)) {
        sh1107_set_pixel(x + col, y + row, true);
      }
    }
  }
}

static void draw_separator(uint8_t y) {
  for (int x = 4; x < 124; x += 3) {
    sh1107_set_pixel(x, y, true);
    sh1107_set_pixel(x + 1, y, true);
  }
}

static void draw_time_display(uint8_t y, const char *time_str) {
  // Extract time part (HH:MM:SS)
  const char *time_part = time_str + strlen(time_str) - 8;

  // Center the time
  uint8_t time_width = 8 * 6 * 2;
  uint8_t x = (128 - time_width) / 2;
  sh1107_draw_string(x, y, time_part, 2);
}

static void draw_date_display(uint8_t y, const char *time_str) {
  // Extract date part (DD/MM/YYYY)
  char date_str[11];
  strncpy(date_str, time_str, 10);
  date_str[10] = '\0';

  // Center the date
  uint8_t date_width = 10 * 6;
  uint8_t x = (128 - date_width) / 2;
  sh1107_draw_string(x, y, date_str, 1);
}

static void draw_connection_status(uint8_t y) {
  if (xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  uint8_t icon_x = 20;
  uint8_t text_x = 45;

  switch (g_oled_internet_type) {
  case CONFIG_INTERNET_WIFI:
    draw_wifi_icon_display(icon_x, y);
    if (g_oled_wifi_connected) {
      sh1107_draw_string(text_x, y + 2, "WiFi", 2);
      if (blink_state) {
        draw_status_dot(95, y + 3, true);
      }
    } else {
      sh1107_draw_string(text_x, y + 2, "WiFi", 1);
      sh1107_draw_string(text_x, y + 10, "Offline", 1);
    }
    break;

  case CONFIG_INTERNET_LTE:
    draw_lte_icon_display(icon_x, y);
    if (g_oled_lte_connected) {
      sh1107_draw_string(text_x + 5, y + 2, "LTE", 2);
      if (blink_state) {
        draw_status_dot(95, y + 3, true);
      }
    } else {
      sh1107_draw_string(text_x + 5, y + 2, "LTE", 1);
      sh1107_draw_string(text_x + 5, y + 10, "Offline", 1);
    }
    break;

  case CONFIG_INTERNET_ETHERNET:
    if (g_oled_eth_connected) {
      sh1107_draw_string(text_x, y + 2, "Ethernet", 2);
      if (blink_state) {
        draw_status_dot(95, y + 3, true);
      }
    } else {
      sh1107_draw_string(text_x, y + 2, "Ethernet", 1);
      sh1107_draw_string(text_x, y + 10, "Offline", 1);
    }
    break;

  default:
    sh1107_draw_string(text_x, y + 4, "No Network", 1);
    break;
  }

  xSemaphoreGive(g_oled_mutex);
}

static void update_display(void) {
  sh1107_clear_display();

  // Get time
  char time_buffer[32];
  int hour, min;
  get_current_time_string(time_buffer, &hour, &min);

  if (xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(g_oled_rtc_string, time_buffer, 32);
    g_oled_rtc_valid = true;
    xSemaphoreGive(g_oled_mutex);
  }

  // Toggle blink state
  blink_state = !blink_state;

  // Draw border
  draw_border();

  // Layout for 128x128 display:
  // Row 5-12: Date (small)
  draw_date_display(5, time_buffer);

  // Row 18-33: Time (large, centered)
  draw_time_display(18, time_buffer);

  // Row 45: Separator
  draw_separator(45);

  // Row 55-70: Connection status
  draw_connection_status(55);

  // Row 85: Separator
  draw_separator(85);

  // Row 95-105: Footer info
  uint8_t footer_y = 95;
  char footer[32];
  if ((g_oled_internet_type == CONFIG_INTERNET_WIFI && g_oled_wifi_connected) ||
      (g_oled_internet_type == CONFIG_INTERNET_LTE && g_oled_lte_connected) ||
      (g_oled_internet_type == CONFIG_INTERNET_ETHERNET && g_oled_eth_connected)) {
    snprintf(footer, sizeof(footer), "Status: Online");
  } else {
    snprintf(footer, sizeof(footer), "Status: Offline");
  }

  uint8_t footer_width = strlen(footer) * 6;
  uint8_t footer_x = (128 - footer_width) / 2;
  sh1107_draw_string(footer_x, footer_y, footer, 1);

  // Additional info line
  sh1107_draw_string(25, 110, "Gateway Ready", 1);

  // Update display
  sh1107_display();
}

// OLED Monitor Task
static void oled_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "OLED monitor task started");
    
    const TickType_t update_interval = pdMS_TO_TICKS(OLED_UPDATE_INTERVAL_MS);
    
    while (g_oled_task_running) {
        TickType_t start_tick = xTaskGetTickCount();
        
        // Update display
        update_display();
        
        // Calculate actual delay needed
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        TickType_t delay_needed = (elapsed < update_interval) ? 
                                   (update_interval - elapsed) : 0;
        
        if (delay_needed > 0) {
            vTaskDelay(delay_needed);
        }
    }
    
    ESP_LOGI(TAG, "OLED monitor task stopped");
    g_oled_task_handle = NULL;
    vTaskDelete(NULL);
}

// Public API
esp_err_t oled_monitor_task_start(void) {
  if (g_oled_task_running) {
    ESP_LOGW(TAG, "OLED monitor task already running");
    return ESP_OK;
  }

  if (g_oled_mutex == NULL) {
    g_oled_mutex = xSemaphoreCreateMutex();
    if (g_oled_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  esp_err_t ret = sh1107_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SH1107: %s", esp_err_to_name(ret));
    return ret;
  }

  // Splash screen
  sh1107_clear_display();
  draw_border();
  sh1107_draw_string(28, 50, "STARTING", 2);
  sh1107_draw_string(20, 70, "Please wait...", 1);
  sh1107_display();
  vTaskDelay(pdMS_TO_TICKS(1000));

  g_oled_task_running = true;

  BaseType_t task_ret =
      xTaskCreate(oled_monitor_task, "oled_monitor", OLED_TASK_STACK_SIZE, NULL,
                  OLED_TASK_PRIORITY, &g_oled_task_handle);

  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    g_oled_task_running = false;
    sh1107_deinit();
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OLED monitor started");
  return ESP_OK;
}

void oled_monitor_task_stop(void) {
  if (!g_oled_task_running)
    return;

  ESP_LOGI(TAG, "Stopping OLED monitor");
  g_oled_task_running = false;

  if (g_oled_task_handle != NULL) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    g_oled_task_handle = NULL;
  }

  sh1107_clear_display();
  sh1107_display();
  sh1107_deinit();

  if (g_oled_mutex != NULL) {
    vSemaphoreDelete(g_oled_mutex);
    g_oled_mutex = NULL;
  }
}

void oled_monitor_update_wifi(bool connected) {
  if (g_oled_mutex &&
      xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    g_oled_wifi_connected = connected;
    xSemaphoreGive(g_oled_mutex);
    ESP_LOGD(TAG, "WiFi: %s", connected ? "Connected" : "Disconnected");
  }
}

void oled_monitor_update_lte(bool connected) {
  if (g_oled_mutex &&
      xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    g_oled_lte_connected = connected;
    xSemaphoreGive(g_oled_mutex);
    ESP_LOGD(TAG, "LTE: %s", connected ? "Connected" : "Disconnected");
  }
}

void oled_monitor_update_eth(bool connected) {
  if (g_oled_mutex &&
      xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    g_oled_eth_connected = connected;
    xSemaphoreGive(g_oled_mutex);
    ESP_LOGD(TAG, "ETH: %s", connected ? "Connected" : "Disconnected");
  }
}

void oled_monitor_update_internet_type(config_internet_type_t type) {
  if (g_oled_mutex &&
      xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    g_oled_internet_type = type;
    xSemaphoreGive(g_oled_mutex);
    ESP_LOGI(TAG, "Internet type: %d", type);
  }
}
