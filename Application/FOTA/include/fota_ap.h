/**
 * @file fota_ap.h
 * @brief FOTA SoftAP — WAN MCU creates a WiFi AP so the LAN MCU can
 *        reach ThingsBoard directly over WiFi instead of PPP/UART.
 *
 * When a LAN MCU firmware update is needed:
 *   1. WAN MCU calls fota_ap_start() → SoftAP "DA2-FOTA" comes up.
 *   2. LAN MCU connects to the AP as a WiFi station.
 *   3. NAPT routes packets from the AP subnet to the internet interface
 *      (WiFi STA, LTE, or Ethernet — whichever the WAN MCU uses).
 *   4. LAN MCU downloads firmware from ThingsBoard, reboots.
 *   5. WAN MCU calls fota_ap_stop() to tear down the AP.
 *
 * The SSID and password here MUST match FOTA_CONFIG_LAN_WIFI_AP_SSID /
 * FOTA_CONFIG_LAN_WIFI_AP_PASS in fota_lan_config.h on the LAN MCU.
 */

#ifndef FOTA_AP_H
#define FOTA_AP_H

#include "esp_err.h"
#include <stdbool.h>

/* ---- AP credentials (must match LAN MCU's fota_lan_config.h) ---------- */
#define FOTA_AP_SSID      "DA2-FOTA"
#define FOTA_AP_PASS      "da2fota1"   /**< WPA2 requires >= 8 chars */
#define FOTA_AP_CHANNEL   6

/* ---- Public API ------------------------------------------------------- */

/**
 * @brief Start the FOTA SoftAP and enable NAPT internet routing.
 *
 * If the WAN MCU's internet type is WiFi, the AP is added in APSTA mode
 * so the existing STA connection is kept alive.  For LTE / Ethernet the
 * WiFi radio is initialised in pure AP mode.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fota_ap_start(void);

/**
 * @brief Stop the FOTA SoftAP and free resources.
 *
 * If the WAN MCU was in APSTA mode the WiFi is reverted to STA-only.
 * For LTE / Ethernet the WiFi radio is fully shut down.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fota_ap_stop(void);

/**
 * @brief Returns true if the FOTA AP is currently running.
 */
bool fota_ap_is_running(void);

#endif /* FOTA_AP_H */
