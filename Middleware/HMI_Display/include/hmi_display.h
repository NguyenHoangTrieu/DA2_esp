/**
 * @file hmi_display.h
 * @brief HMI Middleware — single-page code-rendered dashboard
 *
 * Target display: TJC3224K024_011  —  240 x 320 px native, mounted LANDSCAPE,
 *                 canvas 320(W) x 240(H), 65 K colours, USART HMI, 115200 baud.
 *
 * The TJC project intentionally contains one blank white page only (`home`).
 * All visible content is rendered by firmware via xstr/fill/line commands.
 */

#ifndef HMI_DISPLAY_H
#define HMI_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Display resolution                                                 */
/* ------------------------------------------------------------------ */
#define HMI_DISP_W   320
#define HMI_DISP_H   240

/* ------------------------------------------------------------------ */
/*  Page IDs                                                           */
/* ------------------------------------------------------------------ */
#define HMI_PAGE_HOME   0

/* ------------------------------------------------------------------ */
/*  TJC event byte codes                                               */
/* ------------------------------------------------------------------ */
#define HMI_EVT_TOUCH     0x65
#define HMI_EVT_STRING    0x70
#define HMI_EVT_NUMBER    0x71
#define HMI_EVT_STARTUP   0x88
#define HMI_EVT_PAGE_CHG  0x66

/* ------------------------------------------------------------------ */
/*  Static UI contract                                                 */
/* ------------------------------------------------------------------ */
#define HMI_DEVICE_NAME      "DATN_GATEWAY"
#define HMI_DATE_STR_LEN     16
#define HMI_TIME_STR_LEN     16
#define HMI_LABEL_STR_LEN    16
#define HMI_URL_STR_LEN      48
#define HMI_HINT_STR_LEN     40

/* ------------------------------------------------------------------ */
/*  RGB565 colour palette (TJC decimal values)                         */
/* ------------------------------------------------------------------ */
#define HMI_COL_BLACK      0u
#define HMI_COL_WHITE      65535u
#define HMI_COL_GRAY       33808u
#define HMI_COL_CYAN       2047u
#define HMI_COL_GREEN      2016u
#define HMI_COL_RED        63488u
#define HMI_COL_YELLOW     65504u
#define HMI_COL_ORANGE     64512u

/* ------------------------------------------------------------------ */
/*  Status structure                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  bat_soc;
    bool     bat_is_charging;
    uint16_t bat_voltage_mv;

    char     date_str[HMI_DATE_STR_LEN];
    char     time_str[HMI_TIME_STR_LEN];

    char     internet_type[HMI_LABEL_STR_LEN];
    bool     internet_connected;

    char     server_type[HMI_LABEL_STR_LEN];
    bool     server_connected;

    char     web_url[HMI_URL_STR_LEN];
    char     web_hint[HMI_HINT_STR_LEN];
} hmi_status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void hmi_display_init(void);
void hmi_display_send(const char *cmd);
void hmi_display_sendf(const char *fmt, ...);
esp_err_t hmi_display_read_string(char *out, size_t out_size,
                                  uint32_t timeout_ms);
void hmi_display_goto_page(uint8_t page);
void hmi_display_refresh_status(const hmi_status_t *s);
void hmi_display_handle_frame(const uint8_t *frame, int len);
uint8_t hmi_display_current_page(void);

#ifdef __cplusplus
}
#endif
#endif /* HMI_DISPLAY_H */
