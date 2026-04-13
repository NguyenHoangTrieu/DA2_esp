/**
 * @file hmi_display.h
 * @brief HMI Middleware — TJC protocol, page management, display rendering
 *
 * Target display: TJC3224T024_011  —  240 x 320 px  portrait, 65 K colours,
 *                 USART HMI (Nextion-compatible), 115200 baud.
 *
 * Layer responsibilities:
 *   • TJC command formatting  (text + 0xFF 0xFF 0xFF terminator)
 *   • Page-level navigation   (home / pgWifi / pgLTE / pgKB)
 *   • Component update functions (battery, WiFi, LTE, Ethernet)
 *   • Touch-event dispatch table
 *   • WiFi / LTE config submission via config_handler queue
 *
 * Portrait home-page layout summary (pixel coordinates):
 *   y=  0..27   Title bar : "DA2 GW" | [j_bat 80px] | t_bat_pct | t_bat_status
 *   y= 28..103  WiFi block: hdr / dot+status+ssid / detail
 *   y=104..179  LTE block : hdr / dot+status+apn  / detail
 *   y=180..230  ETH block : hdr / dot+status+ip
 *   y=276..316  Buttons   : [b_wifi_cfg 110px] [b_lte_cfg 110px]
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
/*  Display resolution (TJC3224T024_011 — portrait)                    */
/* ------------------------------------------------------------------ */
#define HMI_DISP_W   240   /* pixels, horizontal */
#define HMI_DISP_H   320   /* pixels, vertical   */

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

/* Home page touchable component IDs (auto-assigned by TJC Editor order) */
#define HMI_HOME_COMP_WIFI_BTN   19   /* b_wifi_cfg */
#define HMI_HOME_COMP_LTE_BTN    20   /* b_lte_cfg  */

/* WiFi page (pgWifi) touchable component IDs */
#define HMI_WIFI_COMP_BACK       1    /* b_back  -> home            */
#define HMI_WIFI_COMP_SSID       4    /* t_ssid_val (xText)         */
#define HMI_WIFI_COMP_PWD        6    /* t_pwd_val  (xText)         */
#define HMI_WIFI_COMP_AUTH       8    /* b_auth_toggle              */
#define HMI_WIFI_COMP_CANCEL     9    /* b_cancel   -> home         */
#define HMI_WIFI_COMP_SET        10   /* b_set      -> submit       */

/* LTE page (pgLTE) touchable component IDs */
#define HMI_LTE_COMP_BACK        1    /* b_back     -> home         */
#define HMI_LTE_COMP_APN         4    /* t_apn_val  (xText)         */
#define HMI_LTE_COMP_USER        6    /* t_user_val (xText)         */
#define HMI_LTE_COMP_PWD         8    /* t_pwd_val  (xText)         */
#define HMI_LTE_COMP_CANCEL      9    /* b_cancel   -> home         */
#define HMI_LTE_COMP_SET         10   /* b_set      -> submit       */

/* KB page (pgKB) component IDs */
#define HMI_KB_COMP_OK           50   /* b_ctrl_ok     -> confirm   */
#define HMI_KB_COMP_CANCEL       51   /* b_ctrl_cancel -> discard   */

/* ------------------------------------------------------------------ */
/*  RGB565 colour palette (TJC decimal values)                          */
/* ------------------------------------------------------------------ */
#define HMI_COL_BLACK      0u
#define HMI_COL_WHITE      65535u
#define HMI_COL_GRAY       33808u
#define HMI_COL_CYAN       2047u    /* 0x07FF — R=0  G=63 B=31 — bright cyan    */
#define HMI_COL_GREEN      2016u    /* 0x07E0 — R=0  G=63 B=0  — bright green   */
#define HMI_COL_RED        63494u
#define HMI_COL_YELLOW     64992u
#define HMI_COL_ORANGE     64512u
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
