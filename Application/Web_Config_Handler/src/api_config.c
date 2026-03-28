/**
 * @file api_config.c
 * @brief REST handlers for GET /api/config and POST /api/config
 *
 * GET  — reads all NVS-backed globals (thread-safe) and returns JSON.
 * POST — parses incoming JSON, builds wire-format command strings identical
 *         to what uart_handler.c sends, and enqueues them to
 *         g_config_handler_queue with CMD_SOURCE_HTTP.
 *
 * Also handles GET/POST /api/lan_config for BLE/LoRa/Zigbee/RS485.
 */

#include "web_config_handler.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"
#include "stack_handler.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_config";

/* Max body size we accept on POST (matches CONFIG_CMD_MAX_LEN) */
#define API_MAX_BODY_LEN  CONFIG_CMD_MAX_LEN

/* ================================================================
 *  Helper: send JSON error response
 * ================================================================ */
static esp_err_t send_json_error(httpd_req_t *req, int status,
                                 const char *msg)
{
    httpd_resp_set_type(req, "application/json");
    if (status == 400)
        httpd_resp_set_status(req, "400 Bad Request");
    else if (status == 500)
        httpd_resp_set_status(req, "500 Internal Server Error");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ================================================================
 *  GET /api/config — return all WAN configuration as JSON
 * ================================================================ */
esp_err_t api_config_get_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* Read thread-safe copies */
    wifi_config_context_t wifi = {0};
    lte_config_context_t  lte  = {0};
    mqtt_config_context_t mqtt = {0};

    config_get_wifi_safe(&wifi);
    config_get_lte_safe(&lte);
    config_get_mqtt_safe(&mqtt);

    /* HTTP & CoAP are plain globals (only written by config_handler task) */
    extern http_config_data_t g_http_cfg;
    extern coap_config_data_t g_coap_cfg;

    /* Build JSON with cJSON */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json_error(req, 500, "Out of memory");
    }

    /* Internet / server type */
    cJSON_AddNumberToObject(root, "internet_type", (int)g_internet_type);
    cJSON_AddNumberToObject(root, "server_type",   (int)g_server_type);

    /* WiFi */
    cJSON *jw = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(jw, "ssid",      wifi.ssid);
    cJSON_AddStringToObject(jw, "password",  wifi.pass);
    cJSON_AddStringToObject(jw, "username",  wifi.username);
    cJSON_AddNumberToObject(jw, "auth_mode", (int)wifi.auth_mode);

    /* LTE */
    cJSON *jl = cJSON_AddObjectToObject(root, "lte");
    cJSON_AddStringToObject(jl, "modem_name",  lte.modem_name);
    cJSON_AddStringToObject(jl, "apn",         lte.apn);
    cJSON_AddStringToObject(jl, "username",    lte.username);
    cJSON_AddStringToObject(jl, "password",    lte.password);
    cJSON_AddNumberToObject(jl, "comm_type",   (int)lte.comm_type);
    cJSON_AddBoolToObject(jl,   "auto_reconnect", lte.auto_reconnect);
    cJSON_AddNumberToObject(jl, "reconnect_timeout_ms",    (double)lte.reconnect_timeout_ms);
    cJSON_AddNumberToObject(jl, "max_reconnect_attempts",  (double)lte.max_reconnect_attempts);
    cJSON_AddNumberToObject(jl, "pwr_pin",     lte.pwr_pin);
    cJSON_AddNumberToObject(jl, "rst_pin",     lte.rst_pin);

    /* MQTT */
    cJSON *jm = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddStringToObject(jm, "broker_uri",       mqtt.broker_uri);
    cJSON_AddStringToObject(jm, "device_token",     mqtt.device_token);
    cJSON_AddStringToObject(jm, "subscribe_topic",  mqtt.subscribe_topic);
    cJSON_AddStringToObject(jm, "publish_topic",    mqtt.publish_topic);
    cJSON_AddStringToObject(jm, "attribute_topic",  mqtt.attribute_topic);
    cJSON_AddNumberToObject(jm, "keepalive_s",      mqtt.keepalive_s);
    cJSON_AddNumberToObject(jm, "timeout_ms",       (double)mqtt.timeout_ms);

    /* HTTP */
    cJSON *jh = cJSON_AddObjectToObject(root, "http");
    cJSON_AddStringToObject(jh, "server_url",     g_http_cfg.server_url);
    cJSON_AddStringToObject(jh, "auth_token",     g_http_cfg.auth_token);
    cJSON_AddNumberToObject(jh, "port",           g_http_cfg.port);
    cJSON_AddBoolToObject(jh,   "use_tls",        g_http_cfg.use_tls);
    cJSON_AddBoolToObject(jh,   "verify_server",  g_http_cfg.verify_server);
    cJSON_AddNumberToObject(jh, "timeout_ms",     (double)g_http_cfg.timeout_ms);

    /* CoAP */
    cJSON *jc = cJSON_AddObjectToObject(root, "coap");
    cJSON_AddStringToObject(jc, "host",           g_coap_cfg.host);
    cJSON_AddStringToObject(jc, "resource_path",  g_coap_cfg.resource_path);
    cJSON_AddStringToObject(jc, "device_token",   g_coap_cfg.device_token);
    cJSON_AddNumberToObject(jc, "port",           g_coap_cfg.port);
    cJSON_AddBoolToObject(jc,   "use_dtls",       g_coap_cfg.use_dtls);
    cJSON_AddNumberToObject(jc, "ack_timeout_ms", (double)g_coap_cfg.ack_timeout_ms);
    cJSON_AddNumberToObject(jc, "max_retransmit", g_coap_cfg.max_retransmit);
    cJSON_AddNumberToObject(jc, "rpc_poll_interval_ms", (double)g_coap_cfg.rpc_poll_interval_ms);

    /* WAN stack ID (local read — no SPI needed) */
    const char *wan_stack_id = stack_handler_get_module_id(0);
    cJSON *jwan = cJSON_AddObjectToObject(root, "wan");
    cJSON_AddStringToObject(jwan, "stack_wan_id",
                            (wan_stack_id && wan_stack_id[0]) ? wan_stack_id : "100");

    /* LAN stack IDs — query LAN MCU via SPI (2 s timeout) */
    char lan_s1[4] = "000";
    char lan_s2[4] = "000";
    {
        uint8_t *lan_buf = malloc(256);
        if (lan_buf) {
            uint16_t lan_len = 0;
            esp_err_t lan_ret = mcu_lan_handler_request_config_async(
                lan_buf, &lan_len, 255, 2000);
            if (lan_ret == ESP_OK && lan_len > 0) {
                lan_buf[lan_len] = '\0';
                char *p1 = strstr((char *)lan_buf, "stack1_id=");
                if (p1) {
                    p1 += 10;
                    int i = 0;
                    while (i < 3 && p1[i] && p1[i] != '|') { lan_s1[i] = p1[i]; i++; }
                    lan_s1[i] = '\0';
                }
                char *p2 = strstr((char *)lan_buf, "stack2_id=");
                if (p2) {
                    p2 += 10;
                    int i = 0;
                    while (i < 3 && p2[i] && p2[i] != '|') { lan_s2[i] = p2[i]; i++; }
                    lan_s2[i] = '\0';
                }
            }
            free(lan_buf);
        }
    }
    cJSON *jlan = cJSON_AddObjectToObject(root, "lan");
    cJSON_AddStringToObject(jlan, "stack1_id", lan_s1);
    cJSON_AddStringToObject(jlan, "stack2_id", lan_s2);

    /* Serialise */
    const char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        cJSON_Delete(root);
        return send_json_error(req, 500, "JSON serialization failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================
 *  Helper: allocate + enqueue a config command (heap, pointer-pass)
 * ================================================================ */
static esp_err_t enqueue_config_cmd(config_type_t type,
                                    const char *wire_data,
                                    uint16_t wire_len)
{
    config_command_t *cmd = malloc(sizeof(config_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate config_command_t");
        return ESP_ERR_NO_MEM;
    }

    cmd->type     = type;
    cmd->source   = CMD_SOURCE_HTTP;
    cmd->data_len = wire_len;
    memcpy(cmd->raw_data, wire_data, wire_len);
    cmd->raw_data[wire_len] = '\0';

    if (!g_config_handler_queue ||
        xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "Config queue full or NULL");
        free(cmd);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ================================================================
 *  JSON → wire-format builders  (one per config section)
 *  These produce the exact same colon/pipe-separated strings that
 *  the Python desktop app sends over UART.
 * ================================================================ */

static esp_err_t build_wifi_cmd(const cJSON *obj)
{
    const cJSON *ssid = cJSON_GetObjectItem(obj, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(obj, "password");
    if (!cJSON_IsString(ssid)) return ESP_ERR_INVALID_ARG;

    const char *pw   = cJSON_IsString(pass)     ? pass->valuestring     : "";
    const cJSON *user = cJSON_GetObjectItem(obj, "username");
    const cJSON *auth = cJSON_GetObjectItem(obj, "auth_mode");

    const char *uname = cJSON_IsString(user) ? user->valuestring : "";
    const char *amode = "PERSONAL";
    if (cJSON_IsNumber(auth) && auth->valueint == 1)
        amode = "ENTERPRISE";

    char wire[512];
    int  len;
    if (uname[0] != '\0') {
        /* WF:SSID:PASSWORD:USERNAME:AUTH_MODE */
        len = snprintf(wire, sizeof(wire), "WF:%s:%s:%s:%s",
                       ssid->valuestring, pw, uname, amode);
    } else {
        /* WF:SSID:PASSWORD:AUTH_MODE */
        len = snprintf(wire, sizeof(wire), "WF:%s:%s:%s",
                       ssid->valuestring, pw, amode);
    }
    return enqueue_config_cmd(CONFIG_TYPE_WIFI, wire, (uint16_t)len);
}

static esp_err_t build_lte_cmd(const cJSON *obj)
{
    /* LT:MODEM:APN:USER:PASS:COMM:AUTO:TIMEOUT:MAXRETRY:PWR:RST */
    const cJSON *modem = cJSON_GetObjectItem(obj, "modem_name");
    const cJSON *apn   = cJSON_GetObjectItem(obj, "apn");
    if (!cJSON_IsString(apn)) return ESP_ERR_INVALID_ARG;

    const char *mn = cJSON_IsString(modem) ? modem->valuestring : "A7600C1";
    const char *us = "", *pw = "";
    const cJSON *u = cJSON_GetObjectItem(obj, "username");
    const cJSON *p = cJSON_GetObjectItem(obj, "password");
    if (cJSON_IsString(u)) us = u->valuestring;
    if (cJSON_IsString(p)) pw = p->valuestring;

    const cJSON *ct  = cJSON_GetObjectItem(obj, "comm_type");
    const char *comm = "USB";
    if (cJSON_IsNumber(ct) && ct->valueint == 1) comm = "UART";

    const cJSON *ar  = cJSON_GetObjectItem(obj, "auto_reconnect");
    const char *arec = (cJSON_IsBool(ar) && cJSON_IsTrue(ar)) ? "true" : "false";

    const cJSON *rt  = cJSON_GetObjectItem(obj, "reconnect_timeout_ms");
    uint32_t timeout = cJSON_IsNumber(rt) ? (uint32_t)rt->valuedouble : 30000;

    const cJSON *mr  = cJSON_GetObjectItem(obj, "max_reconnect_attempts");
    uint32_t maxr    = cJSON_IsNumber(mr) ? (uint32_t)mr->valuedouble : 0;

    const cJSON *pp  = cJSON_GetObjectItem(obj, "pwr_pin");
    const cJSON *rp  = cJSON_GetObjectItem(obj, "rst_pin");
    const char *ppin = "WK", *rpin = "PE";
    char ppin_buf[4] = {0}, rpin_buf[4] = {0};
    if (cJSON_IsNumber(pp)) {
        if (pp->valueint == 11)      ppin = "WK";
        else if (pp->valueint == 12) ppin = "PE";
        else { snprintf(ppin_buf, sizeof(ppin_buf), "%02d", pp->valueint); ppin = ppin_buf; }
    }
    if (cJSON_IsNumber(rp)) {
        if (rp->valueint == 12)      rpin = "PE";
        else if (rp->valueint == 11) rpin = "WK";
        else { snprintf(rpin_buf, sizeof(rpin_buf), "%02d", rp->valueint); rpin = rpin_buf; }
    }

    char wire[512];
    int len = snprintf(wire, sizeof(wire),
                       "LT:%s:%s:%s:%s:%s:%s:%lu:%lu:%s:%s",
                       mn, apn->valuestring, us, pw,
                       comm, arec,
                       (unsigned long)timeout, (unsigned long)maxr,
                       ppin, rpin);
    return enqueue_config_cmd(CONFIG_TYPE_LTE, wire, (uint16_t)len);
}

static esp_err_t build_mqtt_cmd(const cJSON *obj)
{
    /* MQ:URI|TOKEN|SUB|PUB|ATTR */
    const cJSON *uri  = cJSON_GetObjectItem(obj, "broker_uri");
    const cJSON *tok  = cJSON_GetObjectItem(obj, "device_token");
    const cJSON *sub  = cJSON_GetObjectItem(obj, "subscribe_topic");
    const cJSON *pub  = cJSON_GetObjectItem(obj, "publish_topic");
    const cJSON *attr = cJSON_GetObjectItem(obj, "attribute_topic");
    const cJSON *ka   = cJSON_GetObjectItem(obj, "keepalive_s");
    const cJSON *tmo  = cJSON_GetObjectItem(obj, "timeout_ms");

    if (!cJSON_IsString(uri) || !cJSON_IsString(tok))
        return ESP_ERR_INVALID_ARG;

    uint16_t keepalive = cJSON_IsNumber(ka)  ? (uint16_t)ka->valueint  : 0;
    uint32_t timeout   = cJSON_IsNumber(tmo) ? (uint32_t)tmo->valuedouble : 0;

    char wire[1024];
    int len = snprintf(wire, sizeof(wire),
                       "MQ:%s|%s|%s|%s|%s|%u|%lu",
                       uri->valuestring,
                       tok->valuestring,
                       cJSON_IsString(sub)  ? sub->valuestring  : "",
                       cJSON_IsString(pub)  ? pub->valuestring  : "",
                       cJSON_IsString(attr) ? attr->valuestring : "",
                       keepalive,
                       (unsigned long)timeout);
    return enqueue_config_cmd(CONFIG_TYPE_MQTT, wire, (uint16_t)len);
}

static esp_err_t build_http_cmd(const cJSON *obj)
{
    /* HP:URL|AUTH_TOKEN|PORT|USE_TLS|VERIFY|TIMEOUT_MS */
    const cJSON *url  = cJSON_GetObjectItem(obj, "server_url");
    if (!cJSON_IsString(url)) return ESP_ERR_INVALID_ARG;

    const cJSON *tok  = cJSON_GetObjectItem(obj, "auth_token");
    const cJSON *port = cJSON_GetObjectItem(obj, "port");
    const cJSON *tls  = cJSON_GetObjectItem(obj, "use_tls");
    const cJSON *vfy  = cJSON_GetObjectItem(obj, "verify_server");
    const cJSON *tmo  = cJSON_GetObjectItem(obj, "timeout_ms");

    char wire[512];
    int len = snprintf(wire, sizeof(wire),
                       "HP:%s|%s|%d|%d|%d|%lu",
                       url->valuestring,
                       cJSON_IsString(tok) ? tok->valuestring : "",
                       cJSON_IsNumber(port) ? port->valueint : 80,
                       (cJSON_IsBool(tls) && cJSON_IsTrue(tls)) ? 1 : 0,
                       (cJSON_IsBool(vfy) && cJSON_IsTrue(vfy)) ? 1 : 0,
                       (unsigned long)(cJSON_IsNumber(tmo) ? (uint32_t)tmo->valuedouble : 10000));
    return enqueue_config_cmd(CONFIG_TYPE_HTTP, wire, (uint16_t)len);
}

static esp_err_t build_coap_cmd(const cJSON *obj)
{
    /* CP:HOST|RESOURCE|TOKEN|PORT|DTLS|ACK_TIMEOUT|MAX_RTX|RPC_POLL_MS */
    const cJSON *host = cJSON_GetObjectItem(obj, "host");
    if (!cJSON_IsString(host)) return ESP_ERR_INVALID_ARG;

    const cJSON *res  = cJSON_GetObjectItem(obj, "resource_path");
    const cJSON *tok  = cJSON_GetObjectItem(obj, "device_token");
    const cJSON *port = cJSON_GetObjectItem(obj, "port");
    const cJSON *dtls = cJSON_GetObjectItem(obj, "use_dtls");
    const cJSON *ack  = cJSON_GetObjectItem(obj, "ack_timeout_ms");
    const cJSON *rtx  = cJSON_GetObjectItem(obj, "max_retransmit");
    const cJSON *poll = cJSON_GetObjectItem(obj, "rpc_poll_interval_ms");

    char wire[512];
    int len = snprintf(wire, sizeof(wire),
                       "CP:%s|%s|%s|%d|%d|%lu|%d|%lu",
                       host->valuestring,
                       cJSON_IsString(res)  ? res->valuestring  : "",
                       cJSON_IsString(tok)  ? tok->valuestring  : "",
                       cJSON_IsNumber(port) ? port->valueint : 5683,
                       (cJSON_IsBool(dtls) && cJSON_IsTrue(dtls)) ? 1 : 0,
                       (unsigned long)(cJSON_IsNumber(ack) ? (uint32_t)ack->valuedouble : 2000),
                       cJSON_IsNumber(rtx) ? rtx->valueint : 4,
                       (unsigned long)(cJSON_IsNumber(poll) ? (uint32_t)poll->valuedouble : 1500));
    return enqueue_config_cmd(CONFIG_TYPE_COAP, wire, (uint16_t)len);
}

static esp_err_t build_internet_cmd(const cJSON *obj)
{
    /* obj is just a number: 0=WIFI, 1=LTE, 2=ETHERNET, 3=NBIOT */
    if (!cJSON_IsNumber(obj)) return ESP_ERR_INVALID_ARG;

    const char *type_str;
    switch (obj->valueint) {
    case 0: type_str = "WIFI";     break;
    case 1: type_str = "LTE";      break;
    case 2: type_str = "ETHERNET"; break;
    case 3: type_str = "NBIOT";    break;
    default: return ESP_ERR_INVALID_ARG;
    }

    char wire[16];
    int len = snprintf(wire, sizeof(wire), "IN:%s", type_str);
    return enqueue_config_cmd(CONFIG_TYPE_INTERNET, wire, (uint16_t)len);
}

static esp_err_t build_server_type_cmd(const cJSON *obj)
{
    /* obj is a number: 0=MQTT, 1=CoAP, 2=HTTP */
    if (!cJSON_IsNumber(obj)) return ESP_ERR_INVALID_ARG;
    if (obj->valueint < 0 || obj->valueint >= CONFIG_SERVERTYPE_COUNT)
        return ESP_ERR_INVALID_ARG;

    char wire[8];
    int len = snprintf(wire, sizeof(wire), "SV:%d", obj->valueint);
    return enqueue_config_cmd(CONFIG_TYPE_SERVER, wire, (uint16_t)len);
}

/* ================================================================
 *  POST /api/config — accept partial JSON, enqueue commands
 * ================================================================ */
esp_err_t api_config_post_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* --- Read body --- */
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > API_MAX_BODY_LEN) {
        return send_json_error(req, 400, "Body too large or empty");
    }

    char *body = malloc((size_t)content_len + 1);
    if (!body) {
        return send_json_error(req, 500, "Out of memory");
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received,
                                 content_len - received);
        if (ret <= 0) {
            free(body);
            return send_json_error(req, 400, "Receive error");
        }
        received += ret;
    }
    body[received] = '\0';

    /* --- Parse JSON --- */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_error(req, 400, "Invalid JSON");
    }

    int queued = 0;
    int errors = 0;

    /* Each key triggers the matching command builder */
    cJSON *item;

    item = cJSON_GetObjectItem(root, "internet_type");
    if (item) { (build_internet_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "server_type");
    if (item) { (build_server_type_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "wifi");
    if (item) { (build_wifi_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "lte");
    if (item) { (build_lte_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "mqtt");
    if (item) { (build_mqtt_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "http");
    if (item) { (build_http_cmd(item) == ESP_OK) ? queued++ : errors++; }

    item = cJSON_GetObjectItem(root, "coap");
    if (item) { (build_coap_cmd(item) == ESP_OK) ? queued++ : errors++; }

    cJSON_Delete(root);

    if (queued == 0 && errors == 0) {
        return send_json_error(req, 400, "No recognized config sections");
    }

    /* Response */
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":%s,\"queued\":%d,\"errors\":%d}",
             errors == 0 ? "true" : "false", queued, errors);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ================================================================
 *  GET /api/lan_config — request LAN config via SPI
 * ================================================================ */
esp_err_t api_lan_config_get_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    /* Allocate a buffer for the LAN config response */
    uint8_t *buf = malloc(CONFIG_CMD_MAX_LEN);
    if (!buf) {
        return send_json_error(req, 500, "Out of memory");
    }

    uint16_t out_len = 0;
    esp_err_t ret = mcu_lan_handler_request_config_async(
        buf, &out_len, CONFIG_CMD_MAX_LEN, 5000);

    if (ret != ESP_OK || out_len == 0) {
        free(buf);
        return send_json_error(req, 500, "LAN MCU config request failed");
    }

    /* The response is already a colon-separated text block.
     * Forward it as-is in a JSON wrapper so the frontend can parse it. */
    buf[out_len] = '\0';

    httpd_resp_set_type(req, "application/json");

    /* Wrap the raw LAN config in JSON: {"ok":true,"data":"..."} */
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"data\":\"");
    /* Escape any quotes or backslashes in the raw data */
    for (uint16_t i = 0; i < out_len; i++) {
        char c = (char)buf[i];
        if (c == '"' || c == '\\') {
            char esc[3] = {'\\', c, '\0'};
            httpd_resp_sendstr_chunk(req, esc);
        } else if (c == '\n') {
            httpd_resp_sendstr_chunk(req, "\\n");
        } else if (c == '\r') {
            httpd_resp_sendstr_chunk(req, "\\r");
        } else {
            char ch[2] = {c, '\0'};
            httpd_resp_sendstr_chunk(req, ch);
        }
    }
    httpd_resp_sendstr_chunk(req, "\"}");
    httpd_resp_sendstr_chunk(req, NULL); /* finish chunked */

    free(buf);
    return ESP_OK;
}

/* ================================================================
 *  POST /api/lan_config — forward JSON/command to LAN MCU
 *
 *  Body: {"type": "ble_json", "data": "<json_string>"}
 *  type can be: ble_json | ble_cmd | lora_json | lora_cmd |
 *               zigbee_json | zigbee_cmd | rs485
 * ================================================================ */
esp_err_t api_lan_config_post_handler(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > API_MAX_BODY_LEN) {
        return send_json_error(req, 400, "Body too large or empty");
    }

    char *body = malloc((size_t)content_len + 1);
    if (!body) {
        return send_json_error(req, 500, "Out of memory");
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received,
                                 content_len - received);
        if (ret <= 0) {
            free(body);
            return send_json_error(req, 400, "Receive error");
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_error(req, 400, "Invalid JSON");
    }

    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    const cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(type_item) || !cJSON_IsString(data_item)) {
        cJSON_Delete(root);
        return send_json_error(req, 400, "Missing type or data");
    }

    const char *type_str = type_item->valuestring;
    const char *data_str = data_item->valuestring;
    int data_len = (int)strlen(data_str);

    /* Build the MCU LAN command forwarded to LAN MCU via SPI.
     * WAN MCU strips "ML:" (3 bytes) before forwarding, so LAN MCU
     * receives the part after "ML:".  LAN MCU config_parse_type() maps:
     *   CFBL:JSON:{slot}:{json}   → CONFIG_UPDATE_BLE_JSON
     *   CFLR:JSON:{slot}:{json}   → CONFIG_UPDATE_LORA_JSON
     *   CFZB:JSON:{slot}:{json}   → CONFIG_UPDATE_ZIGBEE_JSON
     *   CFRS:JSON:{slot}:{json}   → CONFIG_UPDATE_RS485_JSON
     *   CFRS:BR:{baud}            → CONFIG_UPDATE_RS485
     *   CFBL:{slot}:{at_cmd}      → CONFIG_UPDATE_BLE_CMD
     *   CFLR:{slot}:{at_cmd}      → CONFIG_UPDATE_LORA_CMD
     *   CFZB:{slot}:{at_cmd}      → CONFIG_UPDATE_ZIGBEE_CMD
     */
    char *wire = malloc((size_t)data_len + 32);
    if (!wire) {
        cJSON_Delete(root);
        return send_json_error(req, 500, "Out of memory");
    }

    int wire_len;
    if (strcmp(type_str, "ble_json") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFBL:JSON:%s", data_str);
    } else if (strcmp(type_str, "ble_cmd") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFBL:%s", data_str);
    } else if (strcmp(type_str, "lora_json") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFLR:JSON:%s", data_str);
    } else if (strcmp(type_str, "lora_cmd") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFLR:%s", data_str);
    } else if (strcmp(type_str, "zigbee_json") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFZB:JSON:%s", data_str);
    } else if (strcmp(type_str, "zigbee_cmd") == 0) {
        wire_len = snprintf(wire, data_len + 32, "ML:CFZB:%s", data_str);
    } else if (strcmp(type_str, "rs485_json") == 0) {
        /* data_str format: "<slot>:<minified_json>" e.g. "0:{...}" */
        wire_len = snprintf(wire, data_len + 32, "ML:CFRS:JSON:%s", data_str);
    } else if (strcmp(type_str, "rs485_baud") == 0) {
        /* data_str: baud rate value, e.g. "115200" */
        wire_len = snprintf(wire, data_len + 32, "ML:CFRS:BR:%s", data_str);
    } else if (strcmp(type_str, "rs485") == 0) {
        /* Legacy: data_str already includes colon-prefixed subcommand */
        wire_len = snprintf(wire, data_len + 32, "ML:CFRS%s", data_str);
    } else {
        free(wire);
        cJSON_Delete(root);
        return send_json_error(req, 400, "Unknown LAN config type");
    }

    esp_err_t ret = enqueue_config_cmd(CONFIG_TYPE_MCU_LAN, wire, (uint16_t)wire_len);
    free(wire);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        return send_json_error(req, 500, "Queue full");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
