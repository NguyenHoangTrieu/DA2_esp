#ifndef ETH_CONNECT_H
#define ETH_CONNECT_H

#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

/* ── W5500 SPI Hardware Config ────────────────────────────────────────────── */
#define ETH_SPI_HOST        SPI3_HOST
#define ETH_SPI_SCLK_GPIO   06
#define ETH_SPI_MOSI_GPIO   05
#define ETH_SPI_MISO_GPIO   07
#define ETH_SPI_CS_GPIO     04
/* W5500 INT#/RST# are routed through the adapter IO expander, not directly
 * to ESP32-S3 GPIO. Use polling mode and perform reset through stack_handler. */
#define ETH_INT_GPIO        (-1)
#define ETH_RST_GPIO        (-1)
#define ETH_SPI_CLOCK_MHZ   40   /* W5500 max 80 MHz; pins via GPIO matrix
                                    on this board cap at ~40 MHz reliably.
                                    Reduce to 25 if 40 fails to init. */

/* ── Static IP configuration (PC direct-connect / no DHCP) ─────────────────
 * Set ETH_USE_STATIC_IP to 1 to bypass DHCP and use a fixed IP.
 * Useful when the PC does not have a working DHCP server (Windows ICS issues)
 * or when a predictable IP is needed for development.
 *
 * Topology:
 *   [PC Ethernet: 192.168.137.1] ── cable ── [W5500: ETH_STATIC_IP_ADDR]
 *   PC must have ICS or IP routing enabled for the gateway to reach internet.
 */
#define ETH_USE_STATIC_IP     1               /* 0 = DHCP (default), 1 = static */
#define ETH_STATIC_IP_ADDR    "192.168.137.2"
#define ETH_STATIC_NETMASK    "255.255.255.0"
#define ETH_STATIC_GW         "192.168.137.1" /* PC Ethernet IP */
#define ETH_STATIC_DNS1       "8.8.8.8"
#define ETH_STATIC_DNS2       "1.1.1.1"

/* Global netif handle — used by other modules (e.g. PPP server) */
extern esp_netif_t *g_eth_netif;

/**
 * @brief Start the Ethernet (W5500 SPI) connection task.
 *        Initialises the SPI bus, installs the W5500 driver, creates the
 *        netif, registers event handlers, and starts the driver.
 */
void eth_connect_task_start(void);

/**
 * @brief Stop the Ethernet connection task and release all resources.
 */
void eth_connect_task_stop(void);

/** @brief Returns true when a DHCP-assigned IP address is available. */
bool eth_is_connected(void);

/** @brief Returns true after the first successful SNTP synchronisation. */
bool eth_is_sntp_synced(void);

#endif /* ETH_CONNECT_H */
