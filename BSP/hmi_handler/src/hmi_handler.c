/**
 * @file hmi_handler.c
 * @brief HMI Display Driver — TJC3224K024_011RN
 *
 * UART2 is shared with the PPP server. When entering HMI mode we:
 *   1. Deinit PPP server to release UART2 (if it was active).
 *   2. Install our own UART driver at 115200 baud.
 *   3. Set GPIO46=HIGH to route UART2 → HMI display.
 *   4. Spawn an RX task that parses touch events and text responses.
 *
 * Config commands (CFWF / CFLT) are posted directly to
 * g_config_handler_queue — no UART round-trip needed.
 */

#include "hmi_handler.h"
#include "config_handler.h"
#include "ppp_server.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HMI";

/* ------------------------------------------------------------------ */
/*  Internal state                                                      */
/* ------------------------------------------------------------------ */

static volatile bool     s_hmi_active  = false;
static volatile bool     s_rx_running  = false;
static TaskHandle_t      s_rx_task     = NULL;

/* Current page tracker */
static volatile uint8_t  s_cur_page    = HMI_PAGE_HOME;

/* LTE modem name filled from WAN stack ID at enter-mode */
static char s_lte_modem[32] = "A7600C1";

/* WiFi auth toggle state on pgWifi (0=PERSONAL, 1=ENTERPRISE) */
static volatile uint8_t s_wifi_auth_mode = 0;

/* Field being edited in keyboard page */
static volatile uint8_t s_kb_field  = 0;   /* 0=SSID,1=WiFiPWD,2=APN,3=LTEUser,4=LTEPWD */
static volatile uint8_t s_kb_caller = 0;   /* 1=pgWifi, 2=pgLTE */

/* ------------------------------------------------------------------ */
/*  Low-level UART helpers                                              */
/* ------------------------------------------------------------------ */

void hmi_send(const char *cmd)
{
    if (!s_hmi_active) return;
    uart_write_bytes(HMI_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(HMI_UART_NUM, HMI_TERM, HMI_TERM_LEN);
}

/**
 * @brief Send a printf-formatted command.
 */
static void hmi_sendf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    hmi_send(buf);
}

/**
 * @brief Read the next complete TJC event frame (terminated by 0xFF 0xFF 0xFF).
 *        Returns number of bytes read (including the 3 terminators), 0 on timeout.
 *        Must be called only from the RX task context (blocking read).
 */
static int hmi_read_frame(uint8_t *buf, size_t buf_size, uint32_t timeout_ms)
{
    int idx       = 0;
    int ff_count  = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t byte;
        int len = uart_read_bytes(HMI_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        if (idx < (int)buf_size - 1) {
            buf[idx++] = byte;
        }

        if (byte == 0xFF) {
            ff_count++;
            if (ff_count >= 3) {
                return idx;   /* complete frame */
            }
        } else {
            ff_count = 0;
        }
    }
    return 0;   /* timeout */
}

/**
 * @brief Synchronous read of a string response (0x70 frame).
 *        Drains any non-string frames before the expected response.
 */
static esp_err_t hmi_read_string(char *out, size_t out_size, uint32_t timeout_ms)
{
    uint8_t frame[128];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint32_t remaining_ms = (deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS;
        if (remaining_ms < 20) remaining_ms = 20;
        int len = hmi_read_frame(frame, sizeof(frame), remaining_ms);
        if (len <= 0) break;

        if (frame[0] == HMI_EVT_STRING) {
            /* Frame: 0x70 [str bytes] 0xFF 0xFF 0xFF */
            int str_len = len - 4;  /* strip header + 3 terminators */
            if (str_len < 0) str_len = 0;
            if ((size_t)str_len >= out_size) str_len = (int)out_size - 1;
            memcpy(out, &frame[1], str_len);
            out[str_len] = '\0';
            return ESP_OK;
        }
        /* skip other events (touch, etc.) */
    }
    out[0] = '\0';
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/*  Config command dispatch                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Post a raw config command string directly to the config_handler queue.
 *        Equivalent to a UART config command from the PC app.
 */
static void hmi_dispatch_config(const char *cmd_str)
{
    if (!g_config_handler_queue) {
        ESP_LOGE(TAG, "config_handler queue not ready");
        return;
    }
    int len = (int)strlen(cmd_str);
    if (len >= CONFIG_CMD_MAX_LEN) {
        ESP_LOGE(TAG, "HMI config command too long (%d bytes)", len);
        return;
    }

    config_type_t type = config_parse_type(cmd_str, len);
    if (type == CONFIG_TYPE_UNKNOWN) {
        ESP_LOGW(TAG, "Unknown HMI command type: %.8s...", cmd_str);
        return;
    }

    config_command_t *cmd = (config_command_t *)malloc(sizeof(config_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "OOM for config command");
        return;
    }
    cmd->type     = type;
    cmd->data_len = (uint16_t)len;
    cmd->source   = CMD_SOURCE_UART;  /* ACK goes to UART0 (not UART2/HMI) */
    memcpy(cmd->raw_data, cmd_str, len);
    cmd->raw_data[len] = '\0';

    if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Config queue full, dropping HMI command");
        free(cmd);
    } else {
        ESP_LOGI(TAG, "HMI → config: %.*s", (len > 40 ? 40 : len), cmd_str);
    }
}

/* ------------------------------------------------------------------ */
/*  Page actions                                                        */
/* ------------------------------------------------------------------ */

static void hmi_open_kb(uint8_t caller_page, uint8_t field, const char *current_val)
{
    s_kb_caller = caller_page;
    s_kb_field  = field;

    hmi_sendf("pgKB.va_caller.val=%u", caller_page);
    hmi_sendf("pgKB.va_field.val=%u",  field);
    hmi_sendf("pgKB.t_input.txt=\"%s\"", current_val ? current_val : "");
    hmi_send("page pgKB");
    s_cur_page = HMI_PAGE_KB;
}

static void hmi_toggle_auth_mode(void)
{
    s_wifi_auth_mode ^= 1;
    if (s_wifi_auth_mode == 0) {
        hmi_send("pgWifi.b_auth_toggle.txt=\"PERSONAL\"");
        hmi_sendf("pgWifi.b_auth_toggle.bco=%u", 2624u);   /* COL_BTN_BLUE */
        hmi_sendf("pgWifi.va_auth.val=0");
    } else {
        hmi_send("pgWifi.b_auth_toggle.txt=\"ENTERPRISE\"");
        hmi_sendf("pgWifi.b_auth_toggle.bco=%u", 4920u);   /* COL_BTN_PRESS */
        hmi_sendf("pgWifi.va_auth.val=1");
    }
}

static void hmi_submit_wifi(void)
{
    char ssid[64] = {0};
    char pwd[64]  = {0};

    hmi_send("get pgWifi.t_ssid_val.txt");
    hmi_read_string(ssid, sizeof(ssid), 500);

    hmi_send("get pgWifi.t_pwd_val.txt");
    hmi_read_string(pwd, sizeof(pwd), 500);

    const char *auth = (s_wifi_auth_mode == 0) ? "PERSONAL" : "ENTERPRISE";

    /* CFWF:<SSID>:<PASSWORD>:<AUTH_MODE> */
    char cfg[256];
    snprintf(cfg, sizeof(cfg), "CFWF:%s:%s:%s", ssid, pwd, auth);
    hmi_dispatch_config(cfg);

    /* CFIN:WIFI  (switch internet type) — sent after a short delay */
    vTaskDelay(pdMS_TO_TICKS(500));
    hmi_dispatch_config("CFIN:WIFI");

    hmi_send("page home");
    s_cur_page = HMI_PAGE_HOME;
    ESP_LOGI(TAG, "WiFi config submitted: SSID='%s' auth=%s", ssid, auth);
}

static void hmi_submit_lte(void)
{
    char apn[64]  = {0};
    char user[64] = {0};
    char pwd[64]  = {0};

    hmi_send("get pgLTE.t_apn_val.txt");
    hmi_read_string(apn,  sizeof(apn),  500);

    hmi_send("get pgLTE.t_user_val.txt");
    hmi_read_string(user, sizeof(user), 500);

    hmi_send("get pgLTE.t_pwd_val.txt");
    hmi_read_string(pwd,  sizeof(pwd),  500);

    /* CFLT:<modem>:<apn>:<user>:<pass>:USB:true:30000:0:05:06 */
    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "CFLT:%s:%s:%s:%s:USB:true:30000:0:05:06",
             s_lte_modem, apn, user, pwd);
    hmi_dispatch_config(cfg);

    vTaskDelay(pdMS_TO_TICKS(500));
    hmi_dispatch_config("CFIN:LTE");

    hmi_send("page home");
    s_cur_page = HMI_PAGE_HOME;
    ESP_LOGI(TAG, "LTE config submitted: modem=%s APN='%s'", s_lte_modem, apn);
}

static void hmi_kb_confirm(void)
{
    char value[64] = {0};
    hmi_send("get pgKB.t_input.txt");
    hmi_read_string(value, sizeof(value), 500);

    /* Write result back to the calling field */
    if (s_kb_caller == HMI_PAGE_WIFI) {
        switch (s_kb_field) {
        case 0: hmi_sendf("pgWifi.t_ssid_val.txt=\"%s\"", value); break;
        case 1: hmi_sendf("pgWifi.t_pwd_val.txt=\"%s\"",  value); break;
        default: break;
        }
        hmi_send("page pgWifi");
        s_cur_page = HMI_PAGE_WIFI;
    } else if (s_kb_caller == HMI_PAGE_LTE) {
        switch (s_kb_field) {
        case 2: hmi_sendf("pgLTE.t_apn_val.txt=\"%s\"",  value); break;
        case 3: hmi_sendf("pgLTE.t_user_val.txt=\"%s\"", value); break;
        case 4: hmi_sendf("pgLTE.t_pwd_val.txt=\"%s\"",  value); break;
        default: break;
        }
        hmi_send("page pgLTE");
        s_cur_page = HMI_PAGE_LTE;
    }
}

/* ------------------------------------------------------------------ */
/*  Touch event dispatcher                                              */
/* ------------------------------------------------------------------ */

static void hmi_handle_touch(uint8_t page, uint8_t comp)
{
    if (page == HMI_PAGE_HOME) {
        if (comp == 13) { hmi_send("page pgWifi"); s_cur_page = HMI_PAGE_WIFI; }
        if (comp == 14) { hmi_send("page pgLTE");  s_cur_page = HMI_PAGE_LTE;  }
    }
    else if (page == HMI_PAGE_WIFI) {
        if (comp == 0 || comp == 8) { hmi_send("page home"); s_cur_page = HMI_PAGE_HOME; }
        if (comp == 9)  hmi_submit_wifi();
        if (comp == 7)  hmi_toggle_auth_mode();
        if (comp == 3)  hmi_open_kb(HMI_PAGE_WIFI, 0, NULL);   /* SSID */
        if (comp == 5)  hmi_open_kb(HMI_PAGE_WIFI, 1, NULL);   /* Password */
    }
    else if (page == HMI_PAGE_LTE) {
        if (comp == 0 || comp == 8) { hmi_send("page home"); s_cur_page = HMI_PAGE_HOME; }
        if (comp == 9)  hmi_submit_lte();
        if (comp == 3)  hmi_open_kb(HMI_PAGE_LTE, 2, NULL);    /* APN */
        if (comp == 5)  hmi_open_kb(HMI_PAGE_LTE, 3, NULL);    /* Username */
        if (comp == 7)  hmi_open_kb(HMI_PAGE_LTE, 4, NULL);    /* Password */
    }
    else if (page == HMI_PAGE_KB) {
        /* b_ctrl_ok = component 50: confirm; b_ctrl_cancel = component 51: discard */
        if (comp == 50) {
            hmi_kb_confirm();
        } else if (comp == 51) {
            /* Cancel — navigate back without saving */
            if (s_kb_caller == HMI_PAGE_WIFI) { hmi_send("page pgWifi"); s_cur_page = HMI_PAGE_WIFI; }
            else                               { hmi_send("page pgLTE");  s_cur_page = HMI_PAGE_LTE;  }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  RX Task                                                             */
/* ------------------------------------------------------------------ */

static void hmi_rx_task(void *arg)
{
    ESP_LOGI(TAG, "RX task started");
    s_rx_running = true;

    uint8_t frame[256];

    while (s_hmi_active) {
        int len = hmi_read_frame(frame, sizeof(frame), 200);
        if (len <= 0) continue;

        switch (frame[0]) {
        case HMI_EVT_TOUCH:
            /* Format: 0x65 [page] [comp] [event=0x01 press / 0x00 release] 0xFF 0xFF 0xFF */
            if (len >= 7 && frame[3] == 0x01) {
                hmi_handle_touch(frame[1], frame[2]);
            }
            break;

        case HMI_EVT_STARTUP:
            ESP_LOGI(TAG, "Display startup event");
            hmi_send("page home");
            s_cur_page = HMI_PAGE_HOME;
            break;

        case HMI_EVT_PAGE_CHANGE:
            if (len >= 6) s_cur_page = frame[1];
            break;

        default:
            /* String/number responses are consumed synchronously in submit handlers */
            break;
        }
    }

    s_rx_running = false;
    ESP_LOGI(TAG, "RX task stopped");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Status refresh                                                      */
/* ------------------------------------------------------------------ */

void hmi_refresh_status(const hmi_status_t *s)
{
    if (!s_hmi_active || s_cur_page != HMI_PAGE_HOME) return;

    char buf[80];

    /* Battery */
    uint16_t bat_col = (s->bat_soc >= 50) ? HMI_COL_GREEN  :
                       (s->bat_soc >= 20) ? HMI_COL_YELLOW :
                       (s->bat_soc >= 10) ? HMI_COL_ORANGE : HMI_COL_RED;

    snprintf(buf, sizeof(buf), "home.j_bat.val=%u",     s->bat_soc);     hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_bat_pct.txt=\"%u%%\"", s->bat_soc); hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_bat_pct.pco=%u", bat_col);        hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.j_bat.pco=%u",     bat_col);        hmi_send(buf);

    /* Charging indicator */
    const char *chrg_txt = s->bat_is_charging ? "Charging ⚡" : "(discharging)";
    snprintf(buf, sizeof(buf), "home.t_bat_status.txt=\"%s\"", chrg_txt);
    hmi_send(buf);
    uint16_t chrg_col = s->bat_is_charging ? HMI_COL_GREEN : HMI_COL_GRAY;
    snprintf(buf, sizeof(buf), "home.t_bat_status.pco=%u", chrg_col);
    hmi_send(buf);

    /* WiFi */
    uint16_t wifi_col = s->wifi_connected ? HMI_COL_GREEN : HMI_COL_RED;
    snprintf(buf, sizeof(buf), "home.t_wifi_dot.pco=%u",    wifi_col);   hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_wifi_status.pco=%u", wifi_col);   hmi_send(buf);
    hmi_send(s->wifi_connected
        ? "home.t_wifi_status.txt=\"Connected\""
        : "home.t_wifi_status.txt=\"Disconnected\"");
    snprintf(buf, sizeof(buf), "home.t_wifi_ssid.txt=\"%s\"",
             s->wifi_connected ? s->wifi_ssid : "---");                  hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_wifi_detail.txt=\"Signal: %s   Auth: %s\"",
             s->wifi_rssi_str, s->wifi_auth);                            hmi_send(buf);

    /* LTE */
    uint16_t lte_col = s->lte_connected ? HMI_COL_GREEN : HMI_COL_RED;
    snprintf(buf, sizeof(buf), "home.t_lte_dot.pco=%u",    lte_col);    hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_lte_status.pco=%u", lte_col);    hmi_send(buf);
    hmi_send(s->lte_connected
        ? "home.t_lte_status.txt=\"Connected\""
        : "home.t_lte_status.txt=\"Disconnected\"");
    snprintf(buf, sizeof(buf), "home.t_lte_apn.txt=\"%.50s\"",
             (strlen(s->lte_apn) > 0) ? s->lte_apn : "---");           hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_lte_detail.txt=\"Modem: %.15s  CSQ: %.10s\"",
             s->lte_modem, s->lte_csq_str);                             hmi_send(buf);

    /* Save LTE modem name for use in hmi_submit_lte */
    if (strlen(s->lte_modem) > 0) {
        strncpy(s_lte_modem, s->lte_modem, sizeof(s_lte_modem) - 1);
        s_lte_modem[sizeof(s_lte_modem) - 1] = '\0';
    }

    /* Ethernet */
    uint16_t eth_col = s->eth_connected ? HMI_COL_GREEN : HMI_COL_RED;
    snprintf(buf, sizeof(buf), "home.t_eth_dot.pco=%u",    eth_col);    hmi_send(buf);
    snprintf(buf, sizeof(buf), "home.t_eth_status.pco=%u", eth_col);    hmi_send(buf);
    hmi_send(s->eth_connected
        ? "home.t_eth_status.txt=\"Connected\""
        : "home.t_eth_status.txt=\"Disconnected\"");
    snprintf(buf, sizeof(buf), "home.t_eth_ip.txt=\"%.15s\"",
             (strlen(s->eth_ip) > 0) ? s->eth_ip : "---");              hmi_send(buf);
}

/* ------------------------------------------------------------------ */
/*  Mode management                                                     */
/* ------------------------------------------------------------------ */

void hmi_handler_init(void)
{
    s_hmi_active = false;
    s_rx_running = false;
    s_rx_task    = NULL;
    ESP_LOGI(TAG, "HMI handler initialized (inactive)");
}

esp_err_t hmi_enter_mode(void)
{
    if (s_hmi_active) {
        ESP_LOGW(TAG, "Already in HMI mode");
        return ESP_OK;
    }

    /* 1. Deinit PPP server if it is holding UART2 */
    if (ppp_server_is_initialized()) {
        ESP_LOGI(TAG, "Deinitializing PPP server to release UART2");
        ppp_server_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 2. Install UART2 driver at 115200 baud (HMI protocol speed) */
    uart_config_t uart_cfg = {
        .baud_rate  = HMI_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(HMI_UART_NUM,
                                        HMI_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uart_param_config(HMI_UART_NUM, &uart_cfg);
    uart_set_pin(HMI_UART_NUM,
                 HMI_UART_TX_PIN, HMI_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* 3. Route UART2 to HMI display */
    extern void uart_switch_route_to_hmi(void);
    uart_switch_route_to_hmi();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 4. Mark active and start RX task */
    s_hmi_active = true;
    s_cur_page   = HMI_PAGE_HOME;

    xTaskCreate(hmi_rx_task, "hmi_rx", 4096, NULL, 5, &s_rx_task);

    /* 5. Navigate display to home page */
    hmi_send("page home");

    ESP_LOGI(TAG, "HMI mode active");
    return ESP_OK;
}

esp_err_t hmi_exit_mode(void)
{
    if (!s_hmi_active) {
        ESP_LOGW(TAG, "Not in HMI mode");
        return ESP_OK;
    }

    /* 1. Signal RX task to stop */
    s_hmi_active = false;
    /* Wait for RX task to exit (max 500ms) */
    for (int i = 0; i < 25 && s_rx_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_rx_task = NULL;

    /* 2. Route UART2 back to LAN MCU */
    extern void uart_switch_route_to_lan_mcu(void);
    uart_switch_route_to_lan_mcu();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 3. Uninstall UART2 driver */
    uart_driver_delete(HMI_UART_NUM);

    ESP_LOGI(TAG, "HMI mode exited, UART2 returned to LAN MCU");
    return ESP_OK;
}

bool hmi_is_active(void)
{
    return s_hmi_active;
}
