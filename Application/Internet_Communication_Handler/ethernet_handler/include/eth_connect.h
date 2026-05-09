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
#define ETH_SPI_CLOCK_MHZ   25   /* W5500 max is 80 MHz; 25 MHz is safe    */

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
