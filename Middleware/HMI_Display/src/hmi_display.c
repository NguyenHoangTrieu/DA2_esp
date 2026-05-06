/**
 * @file hmi_display.c
 * @brief HMI Middleware -- TJC3224K024_011 (320x240 landscape)
 *
 * Home page uses only 2 TJC button components for touch routing.
 * All text, battery bar, and separator lines are drawn by ESP32 via xstr/fill/line
 * commands to avoid the TJC per-page component limit on this panel.
 *
 * HOME PAGE BUTTONS (must match TJC Editor exactly):
 *   b_wifi_cfg  Button  x=4,  y=172, w=150, h=36   Touch: page pgWifi
 *   b_lte_cfg   Button  x=166,y=172, w=150, h=36   Touch: page pgLTE
 *
 * WiFi / LTE detail pages also keep only one back button component each.
 * All visible content on those pages is drawn with xstr/fill.
 */
#include "hmi_display.h"
#include "hmi_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *TAG = "HMI_DISP";

static volatile uint8_t s_cur_page     = HMI_PAGE_HOME;
static hmi_status_t     s_status_cache = {0};

/* ------------------------------------------------------------------ */
/*  Send helpers                                                        */
/* ------------------------------------------------------------------ */

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
        if (rem_ms < 20) rem_ms = 20;
        int len = hmi_bsp_read_frame(frame, sizeof(frame), (uint32_t)rem_ms);
        if (len <= 0) break;
        if (frame[0] == HMI_EVT_STRING) {
            int str_len = len - 4;
            if (str_len < 0) str_len = 0;
            if ((size_t)str_len >= out_size) str_len = (int)out_size - 1;
            memcpy(out, &frame[1], str_len);
            out[str_len] = '\0';
            return ESP_OK;
        }
    }
    out[0] = '\0';
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/*  Home page: separator lines (redrawn after every page home nav)     */
/* ------------------------------------------------------------------ */

static void home_draw_seps(void)
{
    hmi_display_sendf("line 0,24,319,24,%u",    HMI_COL_GRAY);
    hmi_display_sendf("line 158,26,158,115,%u", HMI_COL_GRAY);
    hmi_display_sendf("line 0,116,319,116,%u",  HMI_COL_GRAY);
    hmi_display_sendf("line 0,141,319,141,%u",  HMI_COL_GRAY);
}

/* ------------------------------------------------------------------ */
/*  Detail pages -- xstr-based with sta=1 (solid background, no pic)  */
/* ------------------------------------------------------------------ */

static void wifi_page_draw(void)
{
    hmi_display_send("fill 0,0,320,200,0");
    hmi_display_send("xstr 0,0,320,24,0,65535,0,1,1,1,\"WiFi Status\"");

    uint16_t col = s_status_cache.wifi_connected ? HMI_COL_GREEN : HMI_COL_RED;
    const char *st = s_status_cache.wifi_connected ? "Connected" : "Disconnected";
    hmi_display_sendf("xstr 4,28,312,20,0,%u,0,0,1,1,\"Status: %s\"", col, st);

    if (s_status_cache.wifi_connected) {
        hmi_display_sendf("xstr 4,52,312,20,0,65535,0,0,1,1,\"SSID:   %.28s\"",
                          s_status_cache.wifi_ssid);
        hmi_display_sendf("xstr 4,76,312,20,0,65535,0,0,1,1,\"Signal: %s\"",
                          s_status_cache.wifi_rssi_str);
        hmi_display_sendf("xstr 4,100,312,20,0,65535,0,0,1,1,\"Auth:   %s\"",
                          s_status_cache.wifi_auth);
    }
    hmi_display_sendf("xstr 4,148,312,20,0,%u,0,0,1,1,\"Configure via BLE/web app\"",
                      HMI_COL_CYAN);
}

static void lte_page_draw(void)
{
    hmi_display_send("fill 0,0,320,200,0");
    hmi_display_send("xstr 0,0,320,24,0,65535,0,1,1,1,\"LTE Status\"");

    uint16_t col = s_status_cache.lte_connected ? HMI_COL_GREEN : HMI_COL_RED;
    const char *st = s_status_cache.lte_connected ? "Connected" : "Disconnected";
    hmi_display_sendf("xstr 4,28,312,20,0,%u,0,0,1,1,\"Status: %s\"", col, st);

    if (s_status_cache.lte_connected) {
        hmi_display_sendf("xstr 4,52,312,20,0,65535,0,0,1,1,\"APN:    %.28s\"",
                          s_status_cache.lte_apn);
        hmi_display_sendf("xstr 4,76,312,20,0,65535,0,0,1,1,\"Modem:  %s\"",
                          s_status_cache.lte_modem);
        hmi_display_sendf("xstr 4,100,312,20,0,65535,0,0,1,1,\"Signal: %s\"",
                          s_status_cache.lte_csq_str);
    }
    hmi_display_sendf("xstr 4,148,312,20,0,%u,0,0,1,1,\"Configure via BLE/web app\"",
                      HMI_COL_CYAN);
}

/* ------------------------------------------------------------------ */
/*  Navigation                                                          */
/* ------------------------------------------------------------------ */

void hmi_display_init(void)
{
    s_cur_page = HMI_PAGE_HOME;
    memset(&s_status_cache, 0, sizeof(s_status_cache));
}

void hmi_display_goto_page(uint8_t page)
{
    static const char * const names[] = {"home", "pgWifi", "pgLTE", "pgKB"};
    if (page >= 4) return;

    ESP_LOGI(TAG, "goto_page %u (%s)", page, names[page]);
    hmi_bsp_drain();
    hmi_display_sendf("page %s", names[page]);
    vTaskDelay(pdMS_TO_TICKS(150));
    s_cur_page = page;

    switch (page) {
    case HMI_PAGE_HOME:
        home_draw_seps();
        hmi_display_refresh_status(&s_status_cache);
        break;
    case HMI_PAGE_WIFI:
        wifi_page_draw();
        break;
    case HMI_PAGE_LTE:
        lte_page_draw();
        break;
    default:
        break;
    }
}

uint8_t hmi_display_current_page(void)
{
    return s_cur_page;
}

/* ------------------------------------------------------------------ */
/*  Home page status refresh -- xstr/fill only                         */
/* ------------------------------------------------------------------ */

void hmi_display_refresh_status(const hmi_status_t *s)
{
    if (!s || s_cur_page != HMI_PAGE_HOME) return;
    memcpy(&s_status_cache, s, sizeof(hmi_status_t));

    ESP_LOGI(TAG, "refresh bat=%u%% wifi=%d lte=%d eth=%d",
             s->bat_soc, s->wifi_connected, s->lte_connected, s->eth_connected);

    char buf[40];

    hmi_display_send("fill 0,0,320,141,0");
    home_draw_seps();
    hmi_display_send("xstr 2,2,82,20,0,65535,0,0,1,1,\"DA2 GW\"");
    hmi_display_send("xstr 4,27,70,20,0,2047,0,0,1,1,\"WiFi\"");
    hmi_display_send("xstr 162,27,70,20,0,2047,0,0,1,1,\"LTE\"");
    hmi_display_send("xstr 4,120,40,18,0,2047,0,0,1,1,\"ETH:\"");

    /* ---- Battery bar + title row ---- */
    uint16_t bat_col = (s->bat_soc >= 50) ? HMI_COL_GREEN  :
                       (s->bat_soc >= 20) ? HMI_COL_YELLOW :
                       (s->bat_soc >= 10) ? HMI_COL_ORANGE : HMI_COL_RED;
    uint8_t  bar_w   = (uint8_t)((s->bat_soc * 72) / 100);

    hmi_display_sendf("fill 88,6,74,12,%u", HMI_COL_GRAY);
    if (bar_w > 0) {
        hmi_display_sendf("fill 89,7,%u,10,%u", bar_w, bat_col);
    }
    hmi_display_sendf("xstr 166,2,50,20,0,%u,0,0,1,1,\"%u%%\"",
                      bat_col, s->bat_soc);
    hmi_display_sendf("xstr 220,2,70,20,0,%u,0,0,1,1,\"%s\"",
                      s->bat_is_charging ? HMI_COL_GREEN : HMI_COL_GRAY,
                      s->bat_is_charging ? "Chrg" : "Idle");

    /* ---- WiFi column ---- */
    uint16_t wifi_col = s->wifi_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_sendf("xstr 4,51,14,20,0,%u,0,0,1,1,\"*\"", wifi_col);
    hmi_display_sendf("xstr 20,51,92,20,0,%u,0,0,1,1,\"%s\"",
                      s->wifi_connected ? "Connected" : "No WiFi");
    hmi_display_sendf("xstr 4,75,150,18,0,65535,0,0,1,1,\"%.16s\"",
                      s->wifi_connected ? s->wifi_ssid : "---");
    if (s->wifi_connected) {
        snprintf(buf, sizeof(buf), "%.9s %.9s", s->wifi_rssi_str, s->wifi_auth);
        hmi_display_sendf("xstr 4,97,150,18,0,33808,0,0,1,1,\"%s\"", buf);
    } else {
        hmi_display_send("xstr 4,97,150,18,0,33808,0,0,1,1,\"\"");
    }

    /* ---- LTE column ---- */
    uint16_t lte_col = s->lte_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_sendf("xstr 166,51,14,20,0,%u,0,0,1,1,\"*\"", lte_col);
    hmi_display_sendf("xstr 182,51,92,20,0,%u,0,0,1,1,\"%s\"",
                      s->lte_connected ? "Connected" : "No LTE");
    hmi_display_sendf("xstr 166,75,150,18,0,65535,0,0,1,1,\"%.16s\"",
                      s->lte_connected ? s->lte_apn : "---");
    if (s->lte_connected) {
        snprintf(buf, sizeof(buf), "%.9s %s", s->lte_modem, s->lte_csq_str);
        hmi_display_sendf("xstr 166,97,150,18,0,33808,0,0,1,1,\"%s\"", buf);
    } else {
        hmi_display_send("xstr 166,97,150,18,0,33808,0,0,1,1,\"\"");
    }

    /* ---- Ethernet row ---- */
    uint16_t eth_col = s->eth_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_sendf("xstr 48,120,14,18,0,%u,0,0,1,1,\"*\"", eth_col);
    hmi_display_sendf("xstr 64,120,90,18,0,%u,0,0,1,1,\"%s\"",
                      s->eth_connected ? "Connected" : "No ETH");
    if (s->eth_connected) {
        hmi_display_sendf("xstr 158,120,148,18,0,65535,0,0,1,1,\"%.15s\"",
                          s->eth_ip);
    } else {
        hmi_display_send("xstr 158,120,148,18,0,65535,0,0,1,1,\"\"");
    }
}

/* ------------------------------------------------------------------ */
/*  Frame / event dispatcher                                            */
/* ------------------------------------------------------------------ */

void hmi_display_handle_frame(const uint8_t *frame, int len)
{
    if (len <= 0) return;

    switch (frame[0]) {

    case HMI_EVT_TOUCH:
        /* 0x65 [page] [comp] [0x01=press / 0x00=release] 0xFF 0xFF 0xFF */
        if (len < 7 || frame[3] != 0x01) break;
        {
            uint8_t page = frame[1];
            uint8_t comp = frame[2];
            ESP_LOGI(TAG, "Touch page=%u comp=%u", page, comp);
            if (page == HMI_PAGE_HOME) {
                if (comp == HMI_HOME_COMP_WIFI_BTN)
                    hmi_display_goto_page(HMI_PAGE_WIFI);
                else if (comp == HMI_HOME_COMP_LTE_BTN)
                    hmi_display_goto_page(HMI_PAGE_LTE);
            } else if (page == HMI_PAGE_WIFI || page == HMI_PAGE_LTE) {
                if (comp == 1)
                    hmi_display_goto_page(HMI_PAGE_HOME);
            }
        }
        break;

    case HMI_EVT_STARTUP:
        ESP_LOGI(TAG, "Display startup event -- going home");
        hmi_display_goto_page(HMI_PAGE_HOME);
        break;

    case HMI_EVT_PAGE_CHG:
        if (len >= 5) {
            s_cur_page = frame[1];
            ESP_LOGI(TAG, "Page changed to %u", s_cur_page);
        }
        break;

    case 0x01:  /* ACK success  */
    case 0xFF:  /* stray 0xFF   */
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
