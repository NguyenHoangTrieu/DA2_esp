/**
 * @file hmi_display.c
 * @brief HMI Middleware — TJC3224T024_011 display protocol (240x320 portrait)
 *
 * Implements the TJC command protocol, portrait-layout component updates,
 * touch-event dispatch, and WiFi/LTE config submission.
 *
 * Portrait home-page component table (TJC Editor assignment order):
 *   ID  Name              Type      x    y     w    h   Notes
 *    0  (page bg)         page      -    -     -    -   black bg
 *    1  t_title           text      2    4    78   22   "DA2 GW" font1 white
 *    2  t_bat_pct         text    166    4    40   22   "85%"  font1 dynamic-col
 *    3  j_bat             progress 82    7    80   14   battery bar dynamic-col
 *    4  t_bat_status      text    208    6    30   18   "Chrg"/"Idle" font0
 *    5  t_wifi_hdr        text      4   30    62   22   "WiFi" font2 CYAN
 *    6  t_wifi_dot        text      4   56    18   22   "●" font1 dynamic-col
 *    7  t_wifi_status     text     24   56    90   22   font1 dynamic-col
 *    8  t_wifi_ssid       text    118   56   118   22   font1 white
 *    9  t_wifi_detail     text      4   80   234   18   font0 gray
 *   10  t_lte_hdr         text      4  106    62   22   "LTE" font2 CYAN
 *   11  t_lte_dot         text      4  130    18   22   "●" font1 dynamic-col
 *   12  t_lte_status      text     24  130    90   22   font1 dynamic-col
 *   13  t_lte_apn         text    118  130   118   22   font1 white
 *   14  t_lte_detail      text      4  154   234   18   font0 gray
 *   15  t_eth_hdr         text      4  182   100   22   "Ethernet" font2 CYAN
 *   16  t_eth_dot         text      4  206    18   22   "●" font1 dynamic-col
 *   17  t_eth_status      text     24  206    90   22   font1 dynamic-col
 *   18  t_eth_ip          text    118  206   118   22   font1 white
 *   19  b_wifi_cfg        button    4  276   110   40   "WiFi CFG" font1
 *   20  b_lte_cfg         button  126  276   110   40   "LTE CFG"  font1
 *
 * WiFi page (pgWifi) component IDs:
 *   1=b_back  4=t_ssid_val(xText)  6=t_pwd_val(xText)
 *   8=b_auth_toggle  9=b_cancel  10=b_set
 *
 * LTE page (pgLTE) component IDs:
 *   1=b_back  4=t_apn_val  6=t_user_val  8=t_pwd_val  9=b_cancel  10=b_set
 *
 * KB page (pgKB):  50=b_ctrl_ok  51=b_ctrl_cancel
 */

#include "hmi_display.h"
#include "hmi_handler.h"        /* BSP: hmi_bsp_write, hmi_bsp_read_frame   */
#include "config_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HMI_DISP";

/* ------------------------------------------------------------------ */
/*  Internal state                                                      */
/* ------------------------------------------------------------------ */

static volatile uint8_t s_cur_page       = HMI_PAGE_HOME;
static volatile uint8_t s_wifi_auth_mode = 0;   /* 0=PERSONAL, 1=ENTERPRISE */
static volatile uint8_t s_kb_field       = 0;   /* 0=SSID,1=WPwd,2=APN,3=User,4=LPwd */
static volatile uint8_t s_kb_caller      = 0;   /* 1=pgWifi, 2=pgLTE */
static char s_lte_modem_cache[32]        = "A7600C1";

/* ------------------------------------------------------------------ */
/*  Command send helpers                                                */
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
            /* Format: 0x70 [string bytes] 0xFF 0xFF 0xFF */
            int str_len = len - 4;  /* strip 1-byte header + 3 terminators */
            if (str_len < 0) str_len = 0;
            if ((size_t)str_len >= out_size) str_len = (int)out_size - 1;
            memcpy(out, &frame[1], str_len);
            out[str_len] = '\0';
            return ESP_OK;
        }
        /* Discard touch / page-change events that arrive before the response */
    }
    out[0] = '\0';
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/*  Navigation                                                          */
/* ------------------------------------------------------------------ */

void hmi_display_init(void)
{
    s_cur_page       = HMI_PAGE_HOME;
    s_wifi_auth_mode = 0;
    s_kb_caller      = 0;
    s_kb_field       = 0;
}

void hmi_display_goto_page(uint8_t page)
{
    static const char * const names[] = {"home", "pgWifi", "pgLTE", "pgKB"};
    if (page < 4) {
        hmi_display_sendf("page %s", names[page]);
        s_cur_page = page;
    }
}

uint8_t hmi_display_current_page(void)
{
    return s_cur_page;
}

/* ------------------------------------------------------------------ */
/*  Home-page status refresh (portrait 240x320)                        */
/* ------------------------------------------------------------------ */

void hmi_display_refresh_status(const hmi_status_t *s)
{
    if (s_cur_page != HMI_PAGE_HOME) return;

    char buf[96];

    /* ---- Battery -------------------------------------------------- */
    uint16_t bat_col = (s->bat_soc >= 50) ? HMI_COL_GREEN  :
                       (s->bat_soc >= 20) ? HMI_COL_YELLOW :
                       (s->bat_soc >= 10) ? HMI_COL_ORANGE : HMI_COL_RED;

    snprintf(buf, sizeof(buf), "home.j_bat.val=%u", s->bat_soc);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.j_bat.pco=%u", bat_col);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_bat_pct.txt=\"%u%%\"", s->bat_soc);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_bat_pct.pco=%u", bat_col);
    hmi_display_send(buf);

    const char *chrg_txt = s->bat_is_charging ? "Chrg" : "Idle";
    uint16_t chrg_col    = s->bat_is_charging ? HMI_COL_GREEN : HMI_COL_GRAY;
    snprintf(buf, sizeof(buf), "home.t_bat_status.txt=\"%s\"", chrg_txt);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_bat_status.pco=%u", chrg_col);
    hmi_display_send(buf);

    /* ---- WiFi ----------------------------------------------------- */
    uint16_t wifi_col = s->wifi_connected ? HMI_COL_GREEN : HMI_COL_RED;

    snprintf(buf, sizeof(buf), "home.t_wifi_dot.pco=%u", wifi_col);
    hmi_display_send(buf);
    hmi_display_send(s->wifi_connected
        ? "home.t_wifi_status.txt=\"Connected\""
        : "home.t_wifi_status.txt=\"No WiFi\"");
    snprintf(buf, sizeof(buf), "home.t_wifi_status.pco=%u", wifi_col);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_wifi_ssid.txt=\"%.14s\"",
             s->wifi_connected ? s->wifi_ssid : "---");
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_wifi_detail.txt=\"%s  %s\"",
             s->wifi_rssi_str, s->wifi_auth);
    hmi_display_send(buf);

    /* ---- LTE ------------------------------------------------------ */
    uint16_t lte_col = s->lte_connected ? HMI_COL_GREEN : HMI_COL_RED;

    snprintf(buf, sizeof(buf), "home.t_lte_dot.pco=%u", lte_col);
    hmi_display_send(buf);
    hmi_display_send(s->lte_connected
        ? "home.t_lte_status.txt=\"Connected\""
        : "home.t_lte_status.txt=\"No LTE\"");
    snprintf(buf, sizeof(buf), "home.t_lte_status.pco=%u", lte_col);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_lte_apn.txt=\"%.14s\"",
             strlen(s->lte_apn) ? s->lte_apn : "---");
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_lte_detail.txt=\"%.12s %.8s\"",
             s->lte_modem, s->lte_csq_str);
    hmi_display_send(buf);

    /* Cache modem name for hmi_display_submit_lte */
    if (s->lte_modem[0]) {
        strncpy(s_lte_modem_cache, s->lte_modem, sizeof(s_lte_modem_cache) - 1);
        s_lte_modem_cache[sizeof(s_lte_modem_cache) - 1] = '\0';
    }

    /* ---- Ethernet ------------------------------------------------- */
    uint16_t eth_col = s->eth_connected ? HMI_COL_GREEN : HMI_COL_RED;

    snprintf(buf, sizeof(buf), "home.t_eth_dot.pco=%u", eth_col);
    hmi_display_send(buf);
    hmi_display_send(s->eth_connected
        ? "home.t_eth_status.txt=\"Connected\""
        : "home.t_eth_status.txt=\"No ETH\"");
    snprintf(buf, sizeof(buf), "home.t_eth_status.pco=%u", eth_col);
    hmi_display_send(buf);
    snprintf(buf, sizeof(buf), "home.t_eth_ip.txt=\"%.15s\"",
             strlen(s->eth_ip) ? s->eth_ip : "---");
    hmi_display_send(buf);
}

/* ------------------------------------------------------------------ */
/*  Config command dispatch                                             */
/* ------------------------------------------------------------------ */

static void dispatch_config(const char *cmd_str)
{
    if (!g_config_handler_queue) {
        ESP_LOGE(TAG, "config_handler queue not ready");
        return;
    }
    int len = (int)strlen(cmd_str);
    if (len >= CONFIG_CMD_MAX_LEN) {
        ESP_LOGE(TAG, "HMI cfg cmd too long (%d bytes)", len);
        return;
    }
    config_type_t type = config_parse_type(cmd_str, len);
    if (type == CONFIG_TYPE_UNKNOWN) {
        ESP_LOGW(TAG, "Unknown HMI cmd: %.8s", cmd_str);
        return;
    }
    config_command_t *cmd = malloc(sizeof(config_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "OOM allocating config command");
        return;
    }
    cmd->type     = type;
    cmd->data_len = (uint16_t)len;
    cmd->source   = CMD_SOURCE_UART;
    memcpy(cmd->raw_data, cmd_str, len);
    cmd->raw_data[len] = '\0';

    if (xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Config queue full, dropping HMI cmd");
        free(cmd);
    } else {
        ESP_LOGI(TAG, "HMI->config: %.*s", len > 60 ? 60 : len, cmd_str);
    }
}

/* ------------------------------------------------------------------ */
/*  Page action helpers                                                 */
/* ------------------------------------------------------------------ */

static void submit_wifi(void)
{
    char ssid[64] = {0}, pwd[64] = {0};

    hmi_display_send("get pgWifi.t_ssid_val.txt");
    hmi_display_read_string(ssid, sizeof(ssid), 500);

    hmi_display_send("get pgWifi.t_pwd_val.txt");
    hmi_display_read_string(pwd, sizeof(pwd), 500);

    const char *auth = s_wifi_auth_mode ? "ENTERPRISE" : "PERSONAL";

    char cfg[256];
    snprintf(cfg, sizeof(cfg), "CFWF:%s:%s:%s", ssid, pwd, auth);
    dispatch_config(cfg);

    vTaskDelay(pdMS_TO_TICKS(500));
    dispatch_config("CFIN:WIFI");

    hmi_display_goto_page(HMI_PAGE_HOME);
    ESP_LOGI(TAG, "WiFi config submitted: SSID='%s' auth=%s", ssid, auth);
}

static void submit_lte(void)
{
    char apn[64] = {0}, user[64] = {0}, pwd[64] = {0};

    hmi_display_send("get pgLTE.t_apn_val.txt");
    hmi_display_read_string(apn,  sizeof(apn),  500);

    hmi_display_send("get pgLTE.t_user_val.txt");
    hmi_display_read_string(user, sizeof(user), 500);

    hmi_display_send("get pgLTE.t_pwd_val.txt");
    hmi_display_read_string(pwd,  sizeof(pwd),  500);

    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "CFLT:%s:%s:%s:%s:USB:true:30000:0:05:06",
             s_lte_modem_cache, apn, user, pwd);
    dispatch_config(cfg);

    vTaskDelay(pdMS_TO_TICKS(500));
    dispatch_config("CFIN:LTE");

    hmi_display_goto_page(HMI_PAGE_HOME);
    ESP_LOGI(TAG, "LTE config submitted: modem=%s APN='%s'",
             s_lte_modem_cache, apn);
}

static void open_kb(uint8_t caller, uint8_t field, const char *cur_val)
{
    s_kb_caller = caller;
    s_kb_field  = field;
    hmi_display_sendf("pgKB.va_caller.val=%u", caller);
    hmi_display_sendf("pgKB.va_field.val=%u", field);
    hmi_display_sendf("pgKB.t_input.txt=\"%s\"",
                      cur_val ? cur_val : "");
    hmi_display_goto_page(HMI_PAGE_KB);
}

static void kb_confirm(void)
{
    char value[64] = {0};
    hmi_display_send("get pgKB.t_input.txt");
    hmi_display_read_string(value, sizeof(value), 500);

    if (s_kb_caller == HMI_PAGE_WIFI) {
        if      (s_kb_field == 0)
            hmi_display_sendf("pgWifi.t_ssid_val.txt=\"%s\"", value);
        else if (s_kb_field == 1)
            hmi_display_sendf("pgWifi.t_pwd_val.txt=\"%s\"",  value);
        hmi_display_goto_page(HMI_PAGE_WIFI);
    } else {
        if      (s_kb_field == 2)
            hmi_display_sendf("pgLTE.t_apn_val.txt=\"%s\"",  value);
        else if (s_kb_field == 3)
            hmi_display_sendf("pgLTE.t_user_val.txt=\"%s\"", value);
        else if (s_kb_field == 4)
            hmi_display_sendf("pgLTE.t_pwd_val.txt=\"%s\"",  value);
        hmi_display_goto_page(HMI_PAGE_LTE);
    }
}

static void toggle_auth_mode(void)
{
    s_wifi_auth_mode ^= 1;
    if (s_wifi_auth_mode == 0) {
        hmi_display_send("pgWifi.b_auth_toggle.txt=\"PERSONAL\"");
        hmi_display_sendf("pgWifi.b_auth_toggle.bco=%u", HMI_COL_BTN_BLUE);
    } else {
        hmi_display_send("pgWifi.b_auth_toggle.txt=\"ENTERPRISE\"");
        hmi_display_sendf("pgWifi.b_auth_toggle.bco=%u", HMI_COL_BTN_PRESS);
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
        /* Format: 0x65 [page] [comp] [0x01=press / 0x00=release] 0xFF 0xFF 0xFF */
        if (len < 7 || frame[3] != 0x01) break;
        {
            uint8_t page = frame[1];
            uint8_t comp = frame[2];

            if (page == HMI_PAGE_HOME) {
                if (comp == HMI_HOME_COMP_WIFI_BTN)
                    hmi_display_goto_page(HMI_PAGE_WIFI);
                if (comp == HMI_HOME_COMP_LTE_BTN)
                    hmi_display_goto_page(HMI_PAGE_LTE);
            }
            else if (page == HMI_PAGE_WIFI) {
                if (comp == HMI_WIFI_COMP_BACK   ||
                    comp == HMI_WIFI_COMP_CANCEL)    hmi_display_goto_page(HMI_PAGE_HOME);
                else if (comp == HMI_WIFI_COMP_SET)  submit_wifi();
                else if (comp == HMI_WIFI_COMP_AUTH) toggle_auth_mode();
                else if (comp == HMI_WIFI_COMP_SSID) open_kb(HMI_PAGE_WIFI, 0, NULL);
                else if (comp == HMI_WIFI_COMP_PWD)  open_kb(HMI_PAGE_WIFI, 1, NULL);
            }
            else if (page == HMI_PAGE_LTE) {
                if (comp == HMI_LTE_COMP_BACK    ||
                    comp == HMI_LTE_COMP_CANCEL)     hmi_display_goto_page(HMI_PAGE_HOME);
                else if (comp == HMI_LTE_COMP_SET)   submit_lte();
                else if (comp == HMI_LTE_COMP_APN)   open_kb(HMI_PAGE_LTE, 2, NULL);
                else if (comp == HMI_LTE_COMP_USER)  open_kb(HMI_PAGE_LTE, 3, NULL);
                else if (comp == HMI_LTE_COMP_PWD)   open_kb(HMI_PAGE_LTE, 4, NULL);
            }
            else if (page == HMI_PAGE_KB) {
                if (comp == HMI_KB_COMP_OK)
                    kb_confirm();
                else if (comp == HMI_KB_COMP_CANCEL)
                    hmi_display_goto_page(s_kb_caller);
            }
        }
        break;

    case HMI_EVT_STARTUP:
        ESP_LOGI(TAG, "Display startup — going home");
        hmi_display_goto_page(HMI_PAGE_HOME);
        break;

    case HMI_EVT_PAGE_CHG:
        if (len >= 6) {
            s_cur_page = frame[1];
            ESP_LOGD(TAG, "Page changed to %u", s_cur_page);
        }
        break;

    default:
        /* String/number responses consumed synchronously in submit_* helpers */
        break;
    }
}
