/**
 * @file hmi_display.c
 * @brief HMI Middleware -- single-page code-rendered dashboard
 */

#include "hmi_display.h"
#include "hmi_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "HMI_DISP";

static volatile uint8_t s_cur_page = HMI_PAGE_HOME;
static bool s_layout_drawn = false;
static hmi_status_t s_status_cache = {0};

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    hmi_display_sendf("fill %d,%d,%d,%d,%u", x, y, w, h, color);
}

static void draw_text_core(int x, int y, int w, int h, uint16_t fg,
                           uint16_t bg, int xcen, const char *text)
{
    hmi_display_sendf("xstr %d,%d,%d,%d,0,%u,%u,%d,1,1,\"%s\"",
                      x, y, w, h, fg, bg, xcen, text ? text : "");
}

static void draw_text_bold(int x, int y, int w, int h, uint16_t fg,
                           uint16_t bg, int xcen, const char *text)
{
    draw_text_core(x, y, w, h, fg, bg, xcen, text);
    draw_text_core(x + 1, y, w, h, fg, bg, xcen, text);
}

static void draw_section_label(int x, int y, const char *text)
{
    draw_text_bold(x, y, 96, 18, HMI_COL_CYAN, HMI_COL_BLACK, 0, text);
}

static void dashboard_draw_static(void)
{
    fill_rect(0, 0, HMI_DISP_W, HMI_DISP_H, HMI_COL_BLACK);
    hmi_display_sendf("line 0,30,319,30,%u", HMI_COL_GRAY);
    hmi_display_sendf("line 0,78,319,78,%u", HMI_COL_GRAY);
    hmi_display_sendf("line 0,146,319,146,%u", HMI_COL_GRAY);
    hmi_display_sendf("line 0,174,319,174,%u", HMI_COL_GRAY);
    hmi_display_sendf("line 0,202,319,202,%u", HMI_COL_GRAY);

    draw_text_bold(0, 6, 320, 18, HMI_COL_WHITE, HMI_COL_BLACK, 1,
                   HMI_DEVICE_NAME);
    draw_section_label(12, 86, "BATTERY");
    draw_section_label(12, 152, "INTERNET");
    draw_section_label(12, 180, "SERVER");
    draw_section_label(12, 208, "CONFIG");
}

static void dashboard_draw_datetime(const hmi_status_t *s)
{
    fill_rect(0, 34, 320, 40, HMI_COL_BLACK);
    draw_text_core(0, 36, 320, 16, HMI_COL_GRAY, HMI_COL_BLACK, 1,
                   s->date_str);
    draw_text_bold(0, 54, 320, 18, HMI_COL_WHITE, HMI_COL_BLACK, 1,
                   s->time_str);
}

static void dashboard_draw_battery(const hmi_status_t *s)
{
    char line_buf[48];
    uint16_t bat_col;
    uint8_t bar_w;

    bat_col = (s->bat_soc >= 50) ? HMI_COL_GREEN
             : (s->bat_soc >= 20) ? HMI_COL_YELLOW
             : (s->bat_soc >= 10) ? HMI_COL_ORANGE
                                  : HMI_COL_RED;
    bar_w = (uint8_t)((s->bat_soc * 172U) / 100U);

    fill_rect(96, 82, 212, 58, HMI_COL_BLACK);
    draw_text_bold(96, 84, 120, 18,
                   s->bat_is_charging ? HMI_COL_GREEN : HMI_COL_GRAY,
                   HMI_COL_BLACK, 0,
                   s->bat_is_charging ? "CHARGING" : "IDLE");
    hmi_display_sendf("xstr 226,82,82,20,0,%u,%u,1,1,1,\"%u%%\"",
                      bat_col, HMI_COL_BLACK, s->bat_soc);

    fill_rect(100, 108, 176, 14, HMI_COL_GRAY);
    fill_rect(101, 109, 174, 12, HMI_COL_BLACK);
    if (bar_w > 0) {
        fill_rect(101, 109, bar_w, 12, bat_col);
    }

    snprintf(line_buf, sizeof(line_buf), "%u mV", s->bat_voltage_mv);
    draw_text_core(100, 124, 176, 16, HMI_COL_WHITE, HMI_COL_BLACK, 0,
                   line_buf);
}

static void dashboard_draw_row_value(int x, int y, int w, const char *value,
                                     uint16_t value_color)
{
    fill_rect(x, y, w, 18, HMI_COL_BLACK);
    draw_text_bold(x, y, w, 18, value_color, HMI_COL_BLACK, 0, value);
}

static void dashboard_draw_internet(const hmi_status_t *s)
{
    char line_buf[48];

    snprintf(line_buf, sizeof(line_buf), "%s  %s", s->internet_type,
             s->internet_connected ? "ONLINE" : "OFFLINE");
    dashboard_draw_row_value(110, 152, 198, line_buf,
                             s->internet_connected ? HMI_COL_GREEN
                                                   : HMI_COL_RED);
}

static void dashboard_draw_server(const hmi_status_t *s)
{
    char line_buf[48];

    snprintf(line_buf, sizeof(line_buf), "%s  %s", s->server_type,
             s->server_connected ? "CONNECTED" : "DISCONNECTED");
    dashboard_draw_row_value(110, 180, 198, line_buf,
                             s->server_connected ? HMI_COL_GREEN
                                                 : HMI_COL_RED);
}

static void dashboard_draw_web(const hmi_status_t *s)
{
    fill_rect(88, 206, 220, 16, HMI_COL_BLACK);
    fill_rect(12, 222, 296, 16, HMI_COL_BLACK);
    draw_text_bold(88, 206, 220, 16, HMI_COL_WHITE, HMI_COL_BLACK, 0,
                   s->web_url);
    draw_text_core(12, 222, 296, 16, HMI_COL_GRAY, HMI_COL_BLACK, 0,
                   s->web_hint);
}

void hmi_display_send(const char *cmd)
{
    ESP_LOGD(TAG, ">> %s", cmd);
    hmi_bsp_write((const uint8_t *)cmd, strlen(cmd));
    hmi_bsp_write((const uint8_t *)HMI_TJC_TERM, HMI_TJC_TERM_LEN);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void hmi_display_sendf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    hmi_display_send(buf);
}

esp_err_t hmi_display_read_string(char *out, size_t out_size,
                                  uint32_t timeout_ms)
{
    uint8_t frame[128];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint32_t rem_ms = (deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS;
        if (rem_ms < 20) {
            rem_ms = 20;
        }

        int len = hmi_bsp_read_frame(frame, sizeof(frame), (uint32_t)rem_ms);
        if (len <= 0) {
            break;
        }
        if (frame[0] == HMI_EVT_STRING) {
            int str_len = len - 4;
            if (str_len < 0) {
                str_len = 0;
            }
            if ((size_t)str_len >= out_size) {
                str_len = (int)out_size - 1;
            }
            memcpy(out, &frame[1], (size_t)str_len);
            out[str_len] = '\0';
            return ESP_OK;
        }
    }

    out[0] = '\0';
    return ESP_ERR_TIMEOUT;
}

void hmi_display_init(void)
{
    s_cur_page = HMI_PAGE_HOME;
    s_layout_drawn = false;
    memset(&s_status_cache, 0, sizeof(s_status_cache));
}

void hmi_display_goto_page(uint8_t page)
{
    if (page != HMI_PAGE_HOME) {
        ESP_LOGW(TAG, "Unsupported page %u for one-page HMI, forcing home", page);
    }

    ESP_LOGI(TAG, "goto_page home");
    hmi_bsp_drain();
    hmi_display_send("page home");
    vTaskDelay(pdMS_TO_TICKS(150));
    s_cur_page = HMI_PAGE_HOME;
    s_layout_drawn = false;
    hmi_display_refresh_status(&s_status_cache);
}

uint8_t hmi_display_current_page(void)
{
    return s_cur_page;
}

void hmi_display_refresh_status(const hmi_status_t *s)
{
    bool force_redraw = false;

    if (!s || s_cur_page != HMI_PAGE_HOME) {
        return;
    }

    ESP_LOGD(TAG, "refresh bat=%u%% inet=%s/%d server=%s/%d time=%s %s",
             s->bat_soc, s->internet_type, s->internet_connected,
             s->server_type, s->server_connected, s->date_str, s->time_str);

    if (!s_layout_drawn) {
        dashboard_draw_static();
        s_layout_drawn = true;
        force_redraw = true;
    }

    if (force_redraw || strcmp(s->date_str, s_status_cache.date_str) != 0 ||
        strcmp(s->time_str, s_status_cache.time_str) != 0 ||
        s->bat_soc != s_status_cache.bat_soc) {
        dashboard_draw_datetime(s);
    }

    if (force_redraw || s->bat_soc != s_status_cache.bat_soc ||
        s->bat_is_charging != s_status_cache.bat_is_charging ||
        s->bat_voltage_mv != s_status_cache.bat_voltage_mv) {
        dashboard_draw_battery(s);
    }

    if (force_redraw || strcmp(s->internet_type, s_status_cache.internet_type) != 0 ||
        s->internet_connected != s_status_cache.internet_connected) {
        dashboard_draw_internet(s);
    }

    if (force_redraw || strcmp(s->server_type, s_status_cache.server_type) != 0 ||
        s->server_connected != s_status_cache.server_connected) {
        dashboard_draw_server(s);
    }

    if (force_redraw || strcmp(s->web_url, s_status_cache.web_url) != 0 ||
        strcmp(s->web_hint, s_status_cache.web_hint) != 0) {
        dashboard_draw_web(s);
    }

    memcpy(&s_status_cache, s, sizeof(hmi_status_t));
}

void hmi_display_handle_frame(const uint8_t *frame, int len)
{
    if (len <= 0) {
        return;
    }

    switch (frame[0]) {
    case HMI_EVT_TOUCH:
        ESP_LOGD(TAG, "Ignoring touch event in display-only mode");
        break;

    case HMI_EVT_STARTUP:
        ESP_LOGI(TAG, "Display startup event -- going home");
        hmi_display_goto_page(HMI_PAGE_HOME);
        break;

    case HMI_EVT_PAGE_CHG:
        if (len >= 5) {
            s_cur_page = HMI_PAGE_HOME;
            s_layout_drawn = false;
            ESP_LOGI(TAG, "Page changed to home");
        }
        break;

    case 0x01:
    case 0xFF:
        break;

    case 0x00:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x1A:
        ESP_LOGW(TAG, "TJC error 0x%02x", frame[0]);
        break;

    default:
        ESP_LOGW(TAG, "Unknown TJC frame 0x%02x len=%d", frame[0], len);
        break;
    }
}
