/**
 * @file web_config_handler.h
 * @brief Web Config Portal — embedded HTTP server for gateway configuration
 *
 * Provides a browser-based configuration interface equivalent to the
 * desktop Python application.  Serves an embedded SPA (index.html) and
 * exposes REST endpoints that push commands into g_config_handler_queue
 * using CMD_SOURCE_HTTP.
 */
#ifndef WEB_CONFIG_HANDLER_H
#define WEB_CONFIG_HANDLER_H

#include "esp_err.h"

typedef enum {
    WEB_MODE_AP,   /* AP mode — captive portal + DNS redirect */
    WEB_MODE_STA,  /* STA mode — normal web server on station IP */
} web_server_mode_t;

/**
 * @brief Start the web configuration server
 * @param mode  WEB_MODE_AP or WEB_MODE_STA
 * @return ESP_OK on success
 */
esp_err_t web_config_handler_start(web_server_mode_t mode);

/**
 * @brief Stop the web configuration server and free resources
 * @return ESP_OK on success
 */
esp_err_t web_config_handler_stop(void);

/**
 * @brief Get current server mode
 */
web_server_mode_t web_config_handler_get_mode(void);

/* ---- API handlers registered by web_server.c ---- */
/* Implemented in api_config.c */
esp_err_t api_config_get_handler(void *req);
esp_err_t api_config_post_handler(void *req);

/* Implemented in api_status.c */
esp_err_t api_status_get_handler(void *req);
esp_err_t api_reboot_post_handler(void *req);

/* Implemented in api_config.c — LAN config */
esp_err_t api_lan_config_get_handler(void *req);
esp_err_t api_lan_config_post_handler(void *req);

/* ---- Captive DNS (AP mode) implemented in captive_dns.c ---- */
/**
 * @brief Start the captive DNS server (AP mode only).
 *        Responds to every DNS query with 192.168.4.1 to trigger the
 *        "Sign in to network" popup on phones and desktops.
 */
void captive_dns_start(void);

/**
 * @brief Stop the captive DNS server.
 */
void captive_dns_stop(void);

#endif /* WEB_CONFIG_HANDLER_H */
