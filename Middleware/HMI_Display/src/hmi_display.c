/**
 * @file hmi_display.c
 * @brief HMI Middleware -- TJC3224K024_011 display protocol (320x240 landscape)
 *
 * Physical module: TJC3224K024_011 (native 240x320) mounted LANDSCAPE (rotated 90 deg).
 * TJC Editor project must be set to LANDSCAPE, which gives usable area 320(W) x 240(H).
 *
 * All home-page content is drawn with xstr/fill/line UART commands -- no named
 * text component fields are needed on the home page. The TJC project only needs
 * 5 touch-button components across 4 pages:
 *
 *   Page 0 (home)  : comp 1 = b_wifi_cfg (x=4,y=172,w=150,h=36)
 *                    comp 2 = b_lte_cfg  (x=166,y=172,w=150,h=36)
 *   Page 1 (pgWifi): comp 1 = b_back     (x=4,y=196,w=312,h=36)
 *   Page 2 (pgLTE) : comp 1 = b_back     (x=4,y=196,w=312,h=36)
 *   Page 3 (pgKB)  : (empty, reserved)
 *
 * HOME PAGE LAYOUT (320 x 240 landscape):
 *
 *   y=  0..23   Title bar : "DA2 GW" | battery bar (x=88..161) | "85%" | "Chrg/Idle"
 *   y= 24       separator line (full width)
 *   y= 26..47   Section headers : "WiFi" (x=0..157) | "LTE" (x=162..319)
 *   y= 50..71   Status rows     : dot + "Connected"  | dot + "Connected"
 *   y= 74..91   SSID / APN row
 *   y= 94..111  Detail row (signal/auth | modem/CSQ)
 *   y=116       separator line (full width)
 *   y=119..137  Ethernet row : "ETH:" | dot | status | IP
 *   y=141       separator line (full width)
 *   y=172..207  TJC button components (drawn by display controller)
 *   Vertical divider: x=158, y=26..115
 *
 * xstr format: xstr x,y,w,h,fontid,pco,bco,xcen,ycen,sta,"text"
 *   sta=0 transparent bg   sta=1 solid bg
 *   xcen: 0=left 1=center 2=right   ycen: 0=top 1=center
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

static volatile uint8_t s_cur_page    = HMI_PAGE_HOME;
static hmi_status_t     s_status_cache = {0};

/* ------------------------------------------------------------------ */
/*  Send helpers                                                        */
/* ------------------------------------------------------------------ */

void hmi_display_send(const char *cmd)
{
    hmi_bsp_write((const uint8_t *)cmd, strlen(cmd));
    hmi_bsp_write((const uint8_t *)HMI_TJC_TERM, HMI_TJC_TERM_LEN);
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
        int len = hmi_bsp_read_frame(frame, sizeof(frame), rem_ms);
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
/*  Home page static frame (separators + section labels)               */
/* ------------------------------------------------------------------ */

static void home_draw_static(void)
{
    /* Clear entire content area (y=0..171, above TJC button area) */
    hmi_display_send("fill 0,0,320,172,0");

    /* Separator lines */
    hmi_display_sendf("line 0,24,319,24,%u",    HMI_COL_GRAY);  /* below title      */
    hmi_display_sendf("line 158,26,158,115,%u", HMI_COL_GRAY);  /* WiFi|LTE divider */
    hmi_display_sendf("line 0,116,319,116,%u",  HMI_COL_GRAY);  /* below WiFi/LTE   */
    hmi_display_sendf("line 0,141,319,141,%u",  HMI_COL_GRAY);  /* below ETH row    */

    /* Static section labels (never change) */
    hmi_display_sendf("xstr 4,27,70,20,0,%u,0,0,1,0,\"WiFi\"",     HMI_COL_CYAN);
    hmi_display_sendf("xstr 162,27,70,20,0,%u,0,0,1,0,\"LTE\"",    HMI_COL_CYAN);
    hmi_display_sendf("xstr 4,120,40,18,0,%u,0,0,1,0,\"ETH:\"",    HMI_COL_CYAN);
}

/* ------------------------------------------------------------------ */
/*  WiFi / LTE detail pages (full-screen xstr, no named components)    */
/* ------------------------------------------------------------------ */

static void wifi_page_draw(void)
{
    hmi_display_send("fill 0,0,320,172,0");
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
    hmi_display_send("fill 0,0,320,172,0");
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

    hmi_display_sendf("page %s", names[page]);
    vTaskDelay(pdMS_TO_TICKS(60));  /* wait for display page clear to finish */
    s_cur_page = page;

    switch (page) {
    case HMI_PAGE_HOME:
        home_draw_static();
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
/*  Home page status refresh (landscape 320x240)                        */
/*                                                                      */
/*  Layout:                                                             */
/*    y=  0..23   Title bar (fill + redraw each call)                  */
/*    y= 26..47   "WiFi" / "LTE" section labels  (static, see above)   */
/*    y= 50..115  WiFi (x=0..157) + LTE (x=162..319) dynamic content   */
/*    y=119..138  ETH row dynamic content                               */
/*    y=172..207  TJC button components (not touched by fill)           */
/* ------------------------------------------------------------------ */

void hmi_display_refresh_status(const hmi_status_t *s)
{
    if (!s || s_cur_page != HMI_PAGE_HOME) return;
    memcpy(&s_status_cache, s, sizeof(hmi_status_t));

    char buf[128];

    /* ---- Title bar (y=0..23) ---------------------------------------- */
    uint16_t bat_col = (s->bat_soc >= 50) ? HMI_COL_GREEN  :
                       (s->bat_soc >= 20) ? HMI_COL_YELLOW :
                       (s->bat_soc >= 10) ? HMI_COL_ORANGE : HMI_COL_RED;

    hmi_display_send("fill 0,0,320,24,0");
    hmi_display_send("xstr 2,2,82,20,0,65535,0,0,1,0,\"DA2 GW\"");

    /* Battery bar: gray bg at x=88..161 (74 px wide) */
    uint8_t bar_w = (uint8_t)((s->bat_soc * 72) / 100);
    hmi_display_sendf("fill 88,6,74,12,%u", HMI_COL_GRAY);
    if (bar_w > 0) {
        hmi_display_sendf("fill 89,7,%u,10,%u", bar_w, bat_col);
    }

    snprintf(buf, sizeof(buf), "%u%%", s->bat_soc);
    hmi_display_sendf("xstr 166,2,50,20,0,%u,0,1,1,0,\"%s\"", bat_col, buf);

    const char *chrg_txt = s->bat_is_charging ? "Chrg" : "Idle";
    uint16_t chrg_col = s->bat_is_charging ? HMI_COL_GREEN : HMI_COL_GRAY;
    hmi_display_sendf("xstr 220,2,70,20,0,%u,0,0,1,0,\"%s\"", chrg_col, chrg_txt);

    /* ---- WiFi column (x=0..157, dynamic rows y=50..115) -------------- */
    uint16_t wifi_col = s->wifi_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_send("fill 0,50,158,66,0");

    hmi_display_sendf("xstr 4,51,14,20,0,%u,0,1,1,0,\"*\"", wifi_col);
    hmi_display_sendf("xstr 20,51,92,20,0,%u,0,0,1,0,\"%s\"",
                      wifi_col, s->wifi_connected ? "Connected" : "No WiFi");
    hmi_display_sendf("xstr 4,75,150,18,0,65535,0,0,1,0,\"%.16s\"",
                      s->wifi_connected ? s->wifi_ssid : "---");
    if (s->wifi_connected) {
        snprintf(buf, sizeof(buf), "%.9s %.9s", s->wifi_rssi_str, s->wifi_auth);
        hmi_display_sendf("xstr 4,97,150,18,0,%u,0,0,1,0,\"%s\"", HMI_COL_GRAY, buf);
    }

    /* ---- LTE column (x=162..319, dynamic rows y=50..115) ------------- */
    uint16_t lte_col = s->lte_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_send("fill 162,50,158,66,0");

    hmi_display_sendf("xstr 166,51,14,20,0,%u,0,1,1,0,\"*\"", lte_col);
    hmi_display_sendf("xstr 182,51,92,20,0,%u,0,0,1,0,\"%s\"",
                      lte_col, s->lte_connected ? "Connected" : "No LTE");
    hmi_display_sendf("xstr 166,75,150,18,0,65535,0,0,1,0,\"%.16s\"",
                      s->lte_connected ? (strlen(s->lte_apn) ? s->lte_apn : "---") : "---");
    if (s->lte_connected) {
        snprintf(buf, sizeof(buf), "%.9s %s", s->lte_modem, s->lte_csq_str);
        hmi_display_sendf("xstr 166,97,150,18,0,%u,0,0,1,0,\"%s\"", HMI_COL_GRAY, buf);
    }

    /* ---- ETH row (y=119..137) ---------------------------------------- */
    uint16_t eth_col = s->eth_connected ? HMI_COL_GREEN : HMI_COL_RED;
    hmi_display_send("fill 46,119,270,20,0");

    hmi_display_sendf("xstr 48,120,14,18,0,%u,0,1,1,0,\"*\"", eth_col);
    hmi_display_sendf("xstr 64,120,90,18,0,%u,0,0,1,0,\"%s\"",
                      eth_col, s->eth_connected ? "Connected" : "No ETH");
    if (s->eth_connected) {
        hmi_display_sendf("xstr 158,120,148,18,0,65535,0,0,1,0,\"%.15s\"", s->eth_ip);
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
            ESP_LOGD(TAG, "Touch page=%u comp=%u", page, comp);

            if (page == HMI_PAGE_HOME) {
                if (comp == HMI_HOME_COMP_WIFI_BTN)
                    hmi_display_goto_page(HMI_PAGE_WIFI);
                else if (comp == HMI_HOME_COMP_LTE_BTN)
                    hmi_display_goto_page(HMI_PAGE_LTE);
            }
            else if (page == HMI_PAGE_WIFI || page == HMI_PAGE_LTE) {
                if (comp == 1)   /* b_back */
                    hmi_display_goto_page(HMI_PAGE_HOME);
            }
        }
        break;

    case HMI_EVT_STARTUP:
        ESP_LOGI(TAG, "Display startup -- going home");
        hmi_display_goto_page(HMI_PAGE_HOME);
        break;

    case HMI_EVT_PAGE_CHG:
        if (len >= 6) {
            s_cur_page = frame[1];
            ESP_LOGD(TAG, "Page changed to %u", s_cur_page);
        }
        break;

    default:
        break;
    }
}
