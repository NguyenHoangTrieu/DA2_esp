#ifndef CONFIG_H
#define CONFIG_H
/* Firmware upgrade URL endpoint */
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL "https://192.168.0.3:8070/hello_world.bin"

/* Enable certificate bundle (default: enabled) */
#define CONFIG_EXAMPLE_USE_CERT_BUNDLE 1

/* Firmware upgrade URL from stdin (set to 1 if URL is "FROM_STDIN") */
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN 0

/* Skip server certificate CN field check (default: disabled) */
#define CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK 0

/* Support firmware upgrade bind specified interface (default: disabled) */
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF 0

/* OTA data bind interface selection (only one should be enabled if binding is used) */
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_STA 0
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF_ETH 0

/* Use static rx buffer for dynamic buffer after TLS handshake (default: disabled) */
#define CONFIG_EXAMPLE_TLS_DYN_BUF_RX_STATIC 0

/* Enable WiFi connection (set to 1 to use WiFi) */
#define CONFIG_EXAMPLE_CONNECT_WIFI 1

/* Enable Ethernet connection (set to 1 to use Ethernet) */
#define CONFIG_EXAMPLE_CONNECT_ETHERNET 0

#if CONFIG_EXAMPLE_CONNECT_WIFI
#define EXAMPLE_NETIF_DESC_STA "example_netif_sta"
#endif

#if CONFIG_EXAMPLE_CONNECT_ETHERNET
#define EXAMPLE_NETIF_DESC_ETH "example_netif_eth"
#endif

/* Enable certificate bundle support in mbedTLS */
#define MBEDTLS_CERTIFICATE_BUNDLE 1

/* Enable dynamic buffer support in mbedTLS (for TLS_DYN_BUF_RX_STATIC option) */
#define MBEDTLS_DYNAMIC_BUFFER 1

#endif /* CONFIG_H */
