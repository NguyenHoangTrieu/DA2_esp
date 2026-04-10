#ifndef FOTA_CONFIG_H
#define FOTA_CONFIG_H

/* ============================================================
 * ThingsBoard OTA Server Configuration (WAN MCU)
 * ============================================================
 *
 * HOW TO SWITCH SERVERS:
 *   Local Raspberry Pi  → set USE_HTTPS=0, HOST="192.168.x.x", PORT=8080
 *   demo.thingsboard.io → set USE_HTTPS=1, HOST="demo.thingsboard.io", PORT=443
 *
 * HOW TO GET THE DEVICE TOKEN:
 *   ThingsBoard UI → Devices → Your Device → Copy Access Token
 *
 * HOW TO UPDATE FIRMWARE:
 *   ThingsBoard UI → OTA Updates → Upload new .bin → assign to Device Profile
 *   The device automatically downloads the latest assigned firmware.
 * ============================================================ */

/* ============================================================
 * ThingsBoard OTA Server Configuration (WAN MCU)
 * ============================================================
 *
 * Set FOTA_CONFIG_FIRMWARE_URL to the full firmware download URL.
 * Works with ThingsBoard, GitHub Releases, any plain HTTP/HTTPS server.
 *
 * Examples:
 *   ThingsBoard local : http://192.168.1.100:8080/api/v1/TOKEN/firmware?title=DA2_esp&version=1.1.2
 *   ThingsBoard cloud : https://demo.thingsboard.io/api/v1/TOKEN/firmware?title=DA2_esp&version=1.1.2
 *   GitHub Release    : https://github.com/USER/REPO/releases/download/v1.1.2/DA2_esp.bin
 *   Custom HTTP       : http://192.168.1.50/ota/DA2_esp.bin
 * ============================================================ */

/* Full firmware download URL — the only setting that needs to change */
#define FOTA_CONFIG_FIRMWARE_URL \
    "http://192.168.1.100:8080/api/v1/Zfdvk6M9rEmw5fBj7TzP/firmware?title=DA2_esp&version=1.1.2"

/* Maximum URL length stored at runtime */
#define FOTA_CONFIG_FIRMWARE_URL_MAX_LEN  256

/* Use cert bundle for https:// URLs with a public CA (e.g. GitHub, demo.thingsboard.io).
 * Leave 0 for plain http:// or self-signed certs. */
#define FOTA_CONFIG_USE_CERT_BUNDLE 0

/* Firmware upgrade URL from stdin (set to 1 if URL is "FROM_STDIN") */
#define FOTA_CONFIG_FIRMWARE_UPGRADE_URL_FROM_STDIN 0

/* Skip TLS certificate CN field check.
 * Set to 1 only when using a local self-signed certificate. */
#define FOTA_CONFIG_SKIP_COMMON_NAME_CHECK 0

/* Skip firmware version check (default: disabled) */
#define FOTA_CONFIG_SKIP_VERSION_CHECK 0

/* Support firmware upgrade bind specified interface (default: disabled) */
#define FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF 0

/* OTA data bind interface selection */
#define FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF_STA 0
#define FOTA_CONFIG_FIRMWARE_UPGRADE_BIND_IF_ETH 0

/* Use static rx buffer for dynamic buffer after TLS handshake */
#define FOTA_CONFIG_TLS_DYN_BUF_RX_STATIC 0

/* Enable WiFi connection */
#define FOTA_CONFIG_CONNECT_WIFI 1

/* Enable Ethernet connection */
#define FOTA_CONFIG_CONNECT_ETHERNET 0

/* OTA Receive Timeout in milliseconds.
 * 30s is plenty for a local HTTP server — file is ~1.6 MB over LAN. */
#define FOTA_CONFIG_OTA_RECV_TIMEOUT 300000

/* TCP connect timeout for connectivity pre-check (ms) */
#define FOTA_CONFIG_CONNECTIVITY_CHECK_TIMEOUT_MS 5000

/* Enable partial HTTP download (for large firmware images) */
#define FOTA_CONFIG_ENABLE_PARTIAL_HTTP_DOWNLOAD 0

/* HTTP request size for partial download (in bytes) */
#define FOTA_CONFIG_HTTP_REQUEST_SIZE 4096

/* Enable OTA resumption feature */
#define FOTA_CONFIG_ENABLE_OTA_RESUMPTION 0

#if FOTA_CONFIG_CONNECT_WIFI
#define NETIF_DESC_STA "netif_sta"
#endif

#if FOTA_CONFIG_CONNECT_ETHERNET
#define NETIF_DESC_ETH "netif_eth"
#endif

/* Enable certificate bundle support in mbedTLS */
#define MBEDTLS_CERTIFICATE_BUNDLE 1

/* Enable dynamic buffer support in mbedTLS */
#define MBEDTLS_DYNAMIC_BUFFER 1

#endif /* FOTA_CONFIG_H */