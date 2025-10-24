#ifndef CONFIG_H
#define CONFIG_H

/* Firmware upgrade URL endpoint */
#define CONFIG_FIRMWARE_UPGRADE_URL "https://github.com/NguyenHoangTrieu/DA2_esp_release/releases/download/V0.0.1/DA2_esp.bin"

/* Enable certificate bundle (default: enabled) */
#define CONFIG_USE_CERT_BUNDLE 1

/* Firmware upgrade URL from stdin (set to 1 if URL is "FROM_STDIN") */
#define CONFIG_FIRMWARE_UPGRADE_URL_FROM_STDIN 0

/* Skip server certificate CN field check (default: disabled) */
#define CONFIG_SKIP_COMMON_NAME_CHECK 0

/* Skip firmware version check (default: disabled) */
#define CONFIG_SKIP_VERSION_CHECK 0

/* Support firmware upgrade bind specified interface (default: disabled) */
#define CONFIG_FIRMWARE_UPGRADE_BIND_IF 0

/* OTA data bind interface selection */
#define CONFIG_FIRMWARE_UPGRADE_BIND_IF_STA 0
#define CONFIG_FIRMWARE_UPGRADE_BIND_IF_ETH 0

/* Use static rx buffer for dynamic buffer after TLS handshake */
#define CONFIG_TLS_DYN_BUF_RX_STATIC 0

/* Enable WiFi connection */
#define CONFIG_CONNECT_WIFI 1

/* Enable Ethernet connection */
#define CONFIG_CONNECT_ETHERNET 0

/* OTA Receive Timeout in milliseconds */
#define CONFIG_OTA_RECV_TIMEOUT 5000

/* Enable partial HTTP download (for large firmware images) */
#define CONFIG_ENABLE_PARTIAL_HTTP_DOWNLOAD 0

/* HTTP request size for partial download (in bytes) */
#define CONFIG_HTTP_REQUEST_SIZE 4096

/* Enable OTA resumption feature */
#define CONFIG_ENABLE_OTA_RESUMPTION 0

#if CONFIG_CONNECT_WIFI
#define NETIF_DESC_STA "netif_sta"
#endif

#if CONFIG_CONNECT_ETHERNET
#define NETIF_DESC_ETH "netif_eth"
#endif

/* Enable certificate bundle support in mbedTLS */
#define MBEDTLS_CERTIFICATE_BUNDLE 1

/* Enable dynamic buffer support in mbedTLS */
#define MBEDTLS_DYNAMIC_BUFFER 1

#endif /* CONFIG_H */
