/**
 * @file oled_monitor_task.c
 * @brief OLED Monitor - Premium Design with Animations
 */

#include "oled_monitor_task.h"
#include "ssd1306_128x64_handler.h"
#include "ds1307_rtc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

static const char *TAG = "OLED_MON";

// Configuration
#define OLED_TASK_STACK_SIZE    4096
#define OLED_TASK_PRIORITY      3
#define OLED_UPDATE_INTERVAL_MS 1000

// Global Variables
bool g_oled_wifi_connected = false;
bool g_oled_lte_connected = false;
char g_oled_rtc_string[32] = "00/00/0000 00:00:00";
bool g_oled_rtc_valid = false;
config_internet_type_t g_oled_internet_type = CONFIG_INTERNET_WIFI;

// Private Variables
static TaskHandle_t g_oled_task_handle = NULL;
static SemaphoreHandle_t g_oled_mutex = NULL;
static bool g_oled_task_running = false;
static uint8_t blink_state = 0;  // For blinking animation

// WiFi Icon - Smooth arcs (16x14 pixels)
static const uint8_t wifi_icon[] = {
    0b00000001, 0b11111000, 0b00000000,  //      ██████
    0b00000110, 0b00000110, 0b00000000,  //    ██      ██
    0b00011000, 0b00000001, 0b10000000,  //   ██         ██
    0b00100000, 0b00000000, 0b01000000,  //  █             █
    0b00000011, 0b11110000, 0b00000000,  //     ████████
    0b00001100, 0b00001100, 0b00000000,  //    ██      ██
    0b00010000, 0b00000010, 0b00000000,  //   █          █
    0b00000001, 0b11100000, 0b00000000,  //      ████
    0b00000110, 0b00011000, 0b00000000,  //     ██    ██
    0b00000000, 0b11000000, 0b00000000,  //         ██
    0b00000001, 0b11100000, 0b00000000,  //      ████
    0b00000001, 0b11100000, 0b00000000,  //      ████
    0b00000000, 0b11000000, 0b00000000,  //        ██
    0b00000000, 0b00000000, 0b00000000,
};

// LTE Icon - Modern 4G with bars (20x14 pixels)
static const uint8_t lte_icon[] = {
    0b01100000, 0b11100000, 0b00000000,  //  ██    ███      row 0
    0b01100001, 0b00110000, 0b00000000,  //  ██   █   ██   row 1
    0b01100001, 0b00000000, 0b00000000,  //  ██   █        row 2
    0b01100001, 0b00000000, 0b00000000,  //  ██   █        row 3
    0b11111001, 0b01110000, 0b00000000,  // █████ █ ███    row 4 (4 bar + G bar)
    0b11111001, 0b01110000, 0b00000000,  // █████ █ ███    row 5
    0b00000101, 0b00011000, 0b00000000,  //      █ █   ██  row 6
    0b00000101, 0b00011000, 0b00000000,  //      █ █   ██  row 7
    0b00000101, 0b00011000, 0b00000000,  //      █ █   ██  row 8
    0b00000100, 0b11100000, 0b00000000,  //      █  ███    row 9 (G bottom)
    0b00000000, 0b00000000, 0b00000000,  //                row 10 (gap)
    0b00000000, 0b01111110, 0b00000000,  //         ██████ row 11 (bars)
    0b00000000, 0b11111111, 0b00000000,  //        ████████row 12
    0b00000000, 0b11111111, 0b00000000,  //        ████████row 13
};

// Battery/Status dot (for animation)
static const uint8_t status_dot_filled[] = {
    0b00111100,  //   ████
    0b01111110,  //  ██████
    0b11111111,  // ████████
    0b11111111,  // ████████
    0b11111111,  // ████████
    0b11111111,  // ████████
    0b01111110,  //  ██████
    0b00111100,  //   ████
};

static const uint8_t status_dot_empty[] = {
    0b00111100,  //   ████
    0b01000010,  //  █    █
    0b10000001,  // █      █
    0b10000001,  // █      █
    0b10000001,  // █      █
    0b10000001,  // █      █
    0b01000010,  //  █    █
    0b00111100,  //   ████
};

// Helper Functions
static void get_current_time_string(char *buffer, int *hour, int *min) {
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(struct tm));
    
    esp_err_t ret = ds1307_read_time(&timeinfo);
    if (ret != ESP_OK) {
        time_t now = time(NULL);
        localtime_r(&now, &timeinfo);
    }
    
    // Validate
    int year = timeinfo.tm_year + 1900;
    if (year > 9999 || year < 2000) year = 2025;
    int month = timeinfo.tm_mon + 1;
    if (month < 1 || month > 12) month = 1;
    int day = timeinfo.tm_mday;
    if (day < 1 || day > 31) day = 1;
    *hour = timeinfo.tm_hour;
    if (*hour < 0 || *hour > 23) *hour = 0;
    *min = timeinfo.tm_min;
    if (*min < 0 || *min > 59) *min = 0;
    int sec = timeinfo.tm_sec;
    if (sec < 0 || sec > 59) sec = 0;
    
    // Format date and time
    snprintf(buffer, 32, "%02d/%02d/%04d %02d:%02d:%02d",
             day, month, year, *hour, *min, sec);
}

static void draw_border(void) {
    // Top and bottom lines
    for (int x = 0; x < 128; x++) {
        ssd1306_set_pixel(x, 0, true);
        ssd1306_set_pixel(x, 63, true);
    }
    // Left and right lines
    for (int y = 0; y < 64; y++) {
        ssd1306_set_pixel(0, y, true);
        ssd1306_set_pixel(127, y, true);
    }
}

static void draw_status_dot(uint8_t x, uint8_t y, bool filled) {
    const uint8_t *dot = filled ? status_dot_filled : status_dot_empty;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if (dot[i] & (0x80 >> j)) {
                ssd1306_set_pixel(x + j, y + i, true);
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
                ssd1306_set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void draw_lte_icon_display(uint8_t x, uint8_t y) {
    for (int row = 0; row < 14; row++) {
        uint32_t bits = ((uint32_t)lte_icon[row * 3] << 16) |
                        ((uint32_t)lte_icon[row * 3 + 1] << 8) |
                        lte_icon[row * 3 + 2];
        for (int col = 0; col < 20; col++) {
            if (bits & (0x800000 >> col)) {
                ssd1306_set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void draw_separator(uint8_t y) {
    for (int x = 4; x < 124; x += 3) {
        ssd1306_set_pixel(x, y, true);
        ssd1306_set_pixel(x + 1, y, true);
    }
}

static void draw_time_display(uint8_t y, const char *time_str) {
    // Extract time part (HH:MM:SS) - last 8 characters
    const char *time_part = time_str + strlen(time_str) - 8;
    
    // Center the time
    uint8_t time_width = 8 * 6 * 2;  // 8 chars * 6 pixels * size 2
    uint8_t x = (128 - time_width) / 2;
    
    ssd1306_draw_string(x, y, time_part, 2);
}

static void draw_date_display(uint8_t y, const char *time_str) {
    // Extract date part (DD/MM/YYYY) - first 10 characters
    char date_str[11];
    strncpy(date_str, time_str, 10);
    date_str[10] = '\0';
    
    // Center the date
    uint8_t date_width = 10 * 6;  // 10 chars * 6 pixels * size 1
    uint8_t x = (128 - date_width) / 2;
    
    ssd1306_draw_string(x, y, date_str, 1);
}

static void draw_connection_status(uint8_t y) {
    if (xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    
    uint8_t icon_x = 10;
    uint8_t text_x = 35;
    
    switch (g_oled_internet_type) {
        case CONFIG_INTERNET_WIFI:
            draw_wifi_icon_display(icon_x, y);
            if (g_oled_wifi_connected) {
                ssd1306_draw_string(text_x, y + 2, "WiFi", 2);
                // Connected dot (blinking)
                if (blink_state) {
                    draw_status_dot(90, y + 3, true);
                }
            } else {
                ssd1306_draw_string(text_x, y + 2, "WiFi", 1);
                ssd1306_draw_string(text_x, y + 10, "Offline", 1);
            }
            break;
            
        case CONFIG_INTERNET_LTE:
            draw_lte_icon_display(icon_x, y);
            if (g_oled_lte_connected) {
                ssd1306_draw_string(text_x + 5, y + 2, "LTE", 2);
                // Connected dot (blinking)
                if (blink_state) {
                    draw_status_dot(90, y + 3, true);
                }
            } else {
                ssd1306_draw_string(text_x + 5, y + 2, "LTE", 1);
                ssd1306_draw_string(text_x + 5, y + 10, "Offline", 1);
            }
            break;
            
        case CONFIG_INTERNET_ETHERNET:
            ssd1306_draw_string(text_x, y + 2, "Ethernet", 2);
            break;
            
        default:
            ssd1306_draw_string(text_x, y + 4, "No Network", 1);
            break;
    }
    
    xSemaphoreGive(g_oled_mutex);
}

static void update_display(void) {
    ssd1306_clear_display();
    
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
    
    // Layout:
    // Row 3-10: Date (small)
    draw_date_display(3, time_buffer);
    
    // Row 12-27: Time (large, centered)
    draw_time_display(12, time_buffer);
    
    // Row 30: Separator
    draw_separator(30);
    
    // Row 35-49: Connection status
    draw_connection_status(35);
    
    // Row 55-61: Footer info
    uint8_t footer_y = 55;
    char footer[32];
    if (g_oled_internet_type == CONFIG_INTERNET_WIFI && g_oled_wifi_connected) {
        snprintf(footer, sizeof(footer), "Status: Online");
    } else if (g_oled_internet_type == CONFIG_INTERNET_LTE && g_oled_lte_connected) {
        snprintf(footer, sizeof(footer), "Status: Online");
    } else {
        snprintf(footer, sizeof(footer), "Status: Offline");
    }
    uint8_t footer_width = strlen(footer) * 6;
    uint8_t footer_x = (128 - footer_width) / 2;
    ssd1306_draw_string(footer_x, footer_y, footer, 1);
    
    // Update display
    ssd1306_display();
}

// ============================================================================
// OLED Monitor Task
// ============================================================================
static void oled_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "OLED monitor task started");
    
    TickType_t last_update = xTaskGetTickCount();
    const TickType_t update_interval = pdMS_TO_TICKS(OLED_UPDATE_INTERVAL_MS);
    
    while (g_oled_task_running) {
        update_display();
        vTaskDelayUntil(&last_update, update_interval);
    }
    
    ESP_LOGI(TAG, "OLED monitor task stopped");
    g_oled_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================
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
    
    esp_err_t ret = ssd1306_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SSD1306: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Splash screen
    ssd1306_clear_display();
    draw_border();
    ssd1306_draw_string(28, 20, "STARTING", 2);
    ssd1306_draw_string(20, 40, "Please wait...", 1);
    ssd1306_display();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    g_oled_task_running = true;
    BaseType_t task_ret = xTaskCreate(
        oled_monitor_task,
        "oled_monitor",
        OLED_TASK_STACK_SIZE,
        NULL,
        OLED_TASK_PRIORITY,
        &g_oled_task_handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        g_oled_task_running = false;
        ssd1306_deinit();
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OLED monitor started");
    return ESP_OK;
}

void oled_monitor_task_stop(void) {
    if (!g_oled_task_running) return;
    
    ESP_LOGI(TAG, "Stopping OLED monitor");
    g_oled_task_running = false;
    
    if (g_oled_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        g_oled_task_handle = NULL;
    }
    
    ssd1306_clear_display();
    ssd1306_display();
    ssd1306_deinit();
    
    if (g_oled_mutex != NULL) {
        vSemaphoreDelete(g_oled_mutex);
        g_oled_mutex = NULL;
    }
}

void oled_monitor_update_wifi(bool connected) {
    if (g_oled_mutex && xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_oled_wifi_connected = connected;
        xSemaphoreGive(g_oled_mutex);
        ESP_LOGI(TAG, "WiFi: %s", connected ? "Connected" : "Disconnected");
    }
}

void oled_monitor_update_lte(bool connected) {
    if (g_oled_mutex && xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_oled_lte_connected = connected;
        xSemaphoreGive(g_oled_mutex);
        ESP_LOGI(TAG, "LTE: %s", connected ? "Connected" : "Disconnected");
    }
}

void oled_monitor_update_internet_type(config_internet_type_t type) {
    if (g_oled_mutex && xSemaphoreTake(g_oled_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_oled_internet_type = type;
        xSemaphoreGive(g_oled_mutex);
        ESP_LOGI(TAG, "Internet type: %d", type);
    }
}
