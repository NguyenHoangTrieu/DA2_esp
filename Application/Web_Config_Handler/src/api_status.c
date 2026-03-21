/**
 * @file api_status.c
 * @brief REST handlers for GET /api/status and POST /api/reboot
 */

#include "web_config_handler.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "api_status";

/* Extern from wifi_connect.c */
extern uint8_t wifi_get_connection_status(void);

/* ================================================================
 *  GET /api/status
 * ================================================================ */
esp_err_t api_status_get_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* Firmware version from app descriptor */
    const esp_app_desc_t *app = esp_app_get_description();

    /* WAN / LAN firmware version */
    uint32_t wan_ver = WAN_FW_VERSION;
    uint32_t lan_ver = mcu_lan_handler_get_lan_fw_version();

    /* Internet status */
    internet_status_t inet = mcu_lan_handler_get_internet_status();
    uint8_t wifi_connected = wifi_get_connection_status();

    /* WiFi RSSI (only valid when connected) */
    int rssi = 0;
    if (wifi_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    /* Uptime in seconds */
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);

    /* RTC time if available */
    char rtc_buf[24] = "";
    mcu_lan_handler_get_rtc(rtc_buf);

    /* Build JSON response */
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "{"
        "\"firmware_version\":\"%s\","
        "\"wan_fw\":\"%u.%u.%u.%u\","
        "\"lan_fw\":\"%u.%u.%u.%u\","
        "\"internet_type\":%d,"
        "\"server_type\":%d,"
        "\"wifi_connected\":%s,"
        "\"wifi_rssi\":%d,"
        "\"internet_online\":%s,"
        "\"uptime_s\":%lu,"
        "\"rtc\":\"%s\","
        "\"free_heap\":%lu"
        "}",
        app->version,
        (unsigned int)((wan_ver >> 24) & 0xFF), (unsigned int)((wan_ver >> 16) & 0xFF),
        (unsigned int)((wan_ver >> 8)  & 0xFF), (unsigned int)(wan_ver & 0xFF),
        (unsigned int)((lan_ver >> 24) & 0xFF), (unsigned int)((lan_ver >> 16) & 0xFF),
        (unsigned int)((lan_ver >> 8)  & 0xFF), (unsigned int)(lan_ver & 0xFF),
        (int)g_internet_type,
        (int)g_server_type,
        wifi_connected ? "true" : "false",
        rssi,
        (inet == INTERNET_STATUS_ONLINE) ? "true" : "false",
        (unsigned long)uptime_s,
        rtc_buf,
        (unsigned long)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* ================================================================
 *  POST /api/reboot
 * ================================================================ */
esp_err_t api_reboot_post_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    ESP_LOGW(TAG, "Reboot requested via web portal");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Rebooting in 500ms\"}");

    /* Delay then restart — give response time to be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; /* unreachable */
}
