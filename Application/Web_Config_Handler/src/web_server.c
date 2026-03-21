/**
 * @file web_server.c
 * @brief HTTP server lifecycle, route registration, and embedded SPA serving
 */

#include "web_config_handler.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "web_server";

/* ===== Embedded SPA (built by Vite, linked via EMBED_TXTFILES) ===== */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* ===== Module state ===== */
static httpd_handle_t s_server = NULL;
static web_server_mode_t s_mode = WEB_MODE_STA;

/* ===== Static handler prototypes ===== */
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t redirect_handler(httpd_req_t *req);

/* ------------------------------------------------------------ */
/*  Serve embedded index.html for root path                     */
/* ------------------------------------------------------------ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

/* ------------------------------------------------------------ */
/*  Catch-all: redirect unknown paths to /  (SPA + captive)     */
/* ------------------------------------------------------------ */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ===== Route table ===== */

static const httpd_uri_t s_routes[] = {
    /* SPA entry point */
    {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL,
    },
    /* WAN config API */
    {
        .uri      = "/api/config",
        .method   = HTTP_GET,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_config_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri      = "/api/config",
        .method   = HTTP_POST,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_config_post_handler,
        .user_ctx = NULL,
    },
    /* LAN config API (BLE/LoRa/Zigbee/RS485 JSON) */
    {
        .uri      = "/api/lan_config",
        .method   = HTTP_GET,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_lan_config_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri      = "/api/lan_config",
        .method   = HTTP_POST,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_lan_config_post_handler,
        .user_ctx = NULL,
    },
    /* Status / reboot */
    {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_status_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri      = "/api/reboot",
        .method   = HTTP_POST,
        .handler  = (esp_err_t (*)(httpd_req_t *))api_reboot_post_handler,
        .user_ctx = NULL,
    },
    /* Captive portal / SPA catch-all — MUST be last */
    {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = redirect_handler,
        .user_ctx = NULL,
    },
};

/* mDNS support disabled — install espressif/mdns component to enable gateway.local */

/* ===== Public API ===== */

esp_err_t web_config_handler_start(web_server_mode_t mode)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_mode = mode;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register all routes */
    for (int i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        httpd_register_uri_handler(s_server, &s_routes[i]);
    }

    ESP_LOGI(TAG, "Web config server started (mode=%s)",
             mode == WEB_MODE_AP ? "AP" : "STA");
    return ESP_OK;
}

esp_err_t web_config_handler_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    httpd_stop(s_server);
    s_server = NULL;

    ESP_LOGI(TAG, "Web config server stopped");
    return ESP_OK;
}

web_server_mode_t web_config_handler_get_mode(void)
{
    return s_mode;
}
