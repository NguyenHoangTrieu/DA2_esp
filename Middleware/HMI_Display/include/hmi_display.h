/**
 * @file hmi_display.h
 * @brief HMI Middleware — TJC protocol, page management, display rendering
 *
 * Target display: TJC3224K024_011  —  240 x 320 px native, mounted LANDSCAPE,
 *                 canvas 320(W) x 240(H), 65 K colours, USART HMI, 115200 baud.
 *
 * Layer responsibilities:
 *   • TJC command formatting  (text + 0xFF 0xFF 0xFF terminator)
 *   • Page-level navigation   (home / pgWifi / pgLTE / pgKB)
 *   • xstr/fill/line drawing  (all status text drawn by ESP32)
 *   • Touch-event dispatch table
 *
 * Landscape home-page layout (320 x 240):
 *   y=  0..23   Title bar : "DA2 GW" | battery bar (x=88..161) | "%" | Chrg/Idle
 *   y= 24       separator
 *   y= 26..115  WiFi (x=0..157) || LTE (x=162..319) two-column
 *   y=116       separator
 *   y=119..137  ETH row
 *   y=141       separator
 *   y=172..207  TJC buttons: [WiFi] comp1  |  [LTE] comp2
 */

#ifndef HMI_DISPLAY_H
#define HMI_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Display resolution (TJC3224K024_011 — landscape / rotated 90°)     */
/* ------------------------------------------------------------------ */
#define HMI_DISP_W   320   /* pixels, horizontal (landscape) */
#define HMI_DISP_H   240   /* pixels, vertical   (landscape) */

/* ------------------------------------------------------------------ */
/*  Page IDs                                                            */
/* ------------------------------------------------------------------ */
#define HMI_PAGE_HOME   0
#define HMI_PAGE_WIFI   1
#define HMI_PAGE_LTE    2
#define HMI_PAGE_KB     3

/* ------------------------------------------------------------------ */
/*  TJC event byte codes                                                */
/* ------------------------------------------------------------------ */
#define HMI_EVT_TOUCH     0x65
#define HMI_EVT_STRING    0x70
#define HMI_EVT_NUMBER    0x71
#define HMI_EVT_STARTUP   0x88
#define HMI_EVT_PAGE_CHG  0x66

/*
 * Home page: 2 button components only.
 * MUST be the first 2 components added in TJC Editor (IDs 1 and 2).
 * All status text is drawn by ESP32 via xstr/fill commands.
 */
#define HMI_HOME_COMP_WIFI_BTN   1    /* b_wifi_cfg -- comp 1 on home page */
#define HMI_HOME_COMP_LTE_BTN    2    /* b_lte_cfg  -- comp 2 on home page */

/*
 * WiFi / LTE status pages: 1 back-button component only (comp ID 1).
 * Page content is drawn by ESP32 via xstr commands.
 * WiFi/LTE configuration is done via BLE app or web interface.
 */
#define HMI_WIFI_COMP_BACK       1    /* b_back -> home */
#define HMI_LTE_COMP_BACK        1    /* b_back -> home */

/* ------------------------------------------------------------------ */
/*  RGB565 colour palette (TJC decimal values)                          */
/* ------------------------------------------------------------------ */
#define HMI_COL_BLACK      0u
#define HMI_COL_WHITE      65535u
#define HMI_COL_GRAY       33808u
#define HMI_COL_CYAN       2047u    /* 0x07FF — R=0  G=63 B=31 — bright cyan    */
#define HMI_COL_GREEN      2016u    /* 0x07E0 — R=0  G=63 B=0  — bright green   */
#define HMI_COL_RED        63488u   /* 0xF800 — R=31 G=0  B=0  — pure red       */
#define HMI_COL_YELLOW     65504u    /* 0xFFE0 — R=255 G=255 B=0   — pure yellow    */
#define HMI_COL_ORANGE     64512u    /* 0xFC00 — R=255 G=128 B=0   — orange         */
#define HMI_COL_BTN_BLUE   2624u
#define HMI_COL_BTN_PRESS  4920u
#define HMI_COL_TITLE_BG   1316u

/* ------------------------------------------------------------------ */
/*  Status structure                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief System status snapshot displayed on the home page.
 *        Populated by Application layer and passed to hmi_display_refresh_status().
 */
typedef struct {
    /* Battery — from BQ27441 / BQ25892 */
    uint8_t  bat_soc;           /* 0-100 %                          */
    bool     bat_is_charging;   /* true while BQ25892 is charging   */
    uint16_t bat_voltage_mv;    /* mV                               */

    /* WiFi */
    bool     wifi_connected;
    char     wifi_ssid[33];
    char     wifi_rssi_str[12]; /* e.g. "-65 dBm"                  */
    char     wifi_auth[16];     /* "PERSONAL" / "ENTERPRISE"        */

    /* LTE */
    bool     lte_connected;
    char     lte_apn[64];
    char     lte_modem[32];     /* e.g. "A7600C1"                  */
    char     lte_csq_str[12];   /* e.g. "18/31"                    */

    /* Ethernet */
    bool     eth_connected;
    char     eth_ip[16];        /* e.g. "192.168.1.50"             */
} hmi_status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Reset internal page tracker and auth-mode flags.
 *        Called from hmi_task_enter_mode() before activating the display.
 */
void hmi_display_init(void);

/**
 * @brief Send one TJC command and append the 3-byte 0xFF terminator.
 * @param cmd  Null-terminated TJC command string (without terminator).
 */
void hmi_display_send(const char *cmd);

/**
 * @brief printf-style helper: formats then calls hmi_display_send().
 */
void hmi_display_sendf(const char *fmt, ...);

/**
 * @brief Blocking read of a TJC string response frame (type 0x70).
 *        Discards all other event frames until the string arrives or
 *        the timeout expires.
 * @param out         Output buffer for the string content.
 * @param out_size    Capacity of out (including NUL terminator).
 * @param timeout_ms  Maximum wait time.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t hmi_display_read_string(char *out, size_t out_size,
                                  uint32_t timeout_ms);

/**
 * @brief Navigate the display to the specified page.
 * @param page  One of HMI_PAGE_HOME / HMI_PAGE_WIFI / HMI_PAGE_LTE / HMI_PAGE_KB.
 */
void hmi_display_goto_page(uint8_t page);

/**
 * @brief Update all home-page components with the supplied status data.
 *        No-op when the display is not showing the home page.
 * @param s  Pointer to current system status.
 */
void hmi_display_refresh_status(const hmi_status_t *s);

/**
 * @brief Process one received TJC event frame (called from RX task).
 * @param frame  Raw bytes including 0xFF 0xFF 0xFF terminators.
 * @param len    Total frame length in bytes.
 */
void hmi_display_handle_frame(const uint8_t *frame, int len);

/**
 * @brief Return the page ID currently tracked by the middleware.
 */
uint8_t hmi_display_current_page(void);

#ifdef __cplusplus
}
#endif
#endif /* HMI_DISPLAY_H */
