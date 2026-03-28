/**
 * @file hmi_handler.h
 * @brief HMI Display Driver — TJC3224K024_011RN (2.4" 320×240 USART HMI)
 *
 * The display is connected to UART2 (TX=GPIO41, RX=GPIO42) via the FSUSB42UMX-TP
 * UART switch (UART_SEL = GPIO_NUM_46). The switch must be routed to HMI
 * (GPIO46=HIGH) before any display communication.
 *
 * Usage:
 *   1. Call hmi_handler_init() once at boot.
 *   2. Call hmi_enter_mode() to activate display (switches UART to HMI path).
 *   3. While active: hmi_refresh_status() updates the home status page.
 *   4. Call hmi_exit_mode() to release UART back to LAN MCU path.
 */

#ifndef HMI_HANDLER_H
#define HMI_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Hardware / Protocol Constants                                       */
/* ------------------------------------------------------------------ */

#define HMI_UART_NUM         2              /* UART_NUM_2 */
#define HMI_UART_TX_PIN      41             /* Shared with PPP server (PPP_UART_TX_PIN) */
#define HMI_UART_RX_PIN      42             /* Shared with PPP server (PPP_UART_RX_PIN) */
#define HMI_UART_BAUD        115200
#define HMI_UART_BUF_SIZE    1024
#define HMI_UART_SWITCH_GPIO 46             /* GPIO46: 0=LAN MCU, 1=HMI LCD */

/* TJC 3-byte command terminator */
#define HMI_TERM        "\xFF\xFF\xFF"
#define HMI_TERM_LEN    3

/* Page IDs */
#define HMI_PAGE_HOME   0
#define HMI_PAGE_WIFI   1
#define HMI_PAGE_LTE    2
#define HMI_PAGE_KB     3

/* Response event codes from display */
#define HMI_EVT_TOUCH        0x65
#define HMI_EVT_STRING       0x70
#define HMI_EVT_NUMBER       0x71
#define HMI_EVT_STARTUP      0x88
#define HMI_EVT_PAGE_CHANGE  0x66

/* RGB565 color values (TJC decimal) */
#define HMI_COL_GREEN    6144u
#define HMI_COL_YELLOW   64992u
#define HMI_COL_ORANGE   64512u
#define HMI_COL_RED      63494u
#define HMI_COL_WHITE    65535u
#define HMI_COL_GRAY     33808u

/* ------------------------------------------------------------------ */
/*  Data Structures                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Status data shown on home page (Page 0).
 *        Populated by the application and passed to hmi_refresh_status().
 */
typedef struct {
    /* Battery — from pwr_source_handler (BQ27441) */
    uint8_t  bat_soc;           /* 0–100 % */
    bool     bat_is_charging;   /* true when BQ25892 CHRG_STAT != 0 */
    uint16_t bat_voltage_mv;    /* mV */

    /* WiFi — from wifi_connect handler */
    bool     wifi_connected;
    char     wifi_ssid[33];     /* max WiFi SSID length */
    char     wifi_rssi_str[12]; /* e.g. "-65 dBm" */
    char     wifi_auth[16];     /* "PERSONAL" or "ENTERPRISE" */

    /* LTE — from lte_connect handler */
    bool     lte_connected;
    char     lte_apn[64];
    char     lte_modem[32];     /* e.g. "A7600C1" */
    char     lte_csq_str[12];   /* e.g. "18/31" */

    /* Ethernet — from eth_connect handler */
    bool     eth_connected;
    char     eth_ip[16];        /* e.g. "192.168.1.50" */
} hmi_status_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief One-time initialization at boot (does NOT start UART or switch GPIO).
 *        Safe to call before hmi_enter_mode().
 */
void hmi_handler_init(void);

/**
 * @brief Enter HMI mode:
 *   1. Deinit PPP server if it holds UART2.
 *   2. Install UART2 driver at 115200 baud.
 *   3. Set GPIO46 HIGH → route UART2 to HMI display.
 *   4. Start display RX task, navigate to home page.
 * @return ESP_OK on success
 */
esp_err_t hmi_enter_mode(void);

/**
 * @brief Exit HMI mode:
 *   1. Stop RX task, navigate display to home page.
 *   2. Set GPIO46 LOW → route UART2 back to LAN MCU.
 *   3. Uninstall UART2 driver.
 * @return ESP_OK on success
 */
esp_err_t hmi_exit_mode(void);

/**
 * @brief Returns true if HMI is currently active.
 */
bool hmi_is_active(void);

/**
 * @brief Refresh home page status fields.
 *        Must be called while hmi_is_active() == true.
 * @param s Pointer to current status data
 */
void hmi_refresh_status(const hmi_status_t *s);

/**
 * @brief Send a formatted command string to the display.
 *        Appends the 3-byte 0xFF terminator automatically.
 * @param cmd Null-terminated command string (without terminator)
 */
void hmi_send(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* HMI_HANDLER_H */
