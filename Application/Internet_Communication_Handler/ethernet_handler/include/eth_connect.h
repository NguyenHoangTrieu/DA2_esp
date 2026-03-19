#ifndef ETH_CONNECT_H
#define ETH_CONNECT_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

/* ── W5500 SPI Hardware Config ──────────────────────────────────────────────
 * TODO: Replace placeholder GPIO numbers with the actual pin assignments
 * from your schematic before flashing.
 * ────────────────────────────────────────────────────────────────────────── */
#define ETH_SPI_HOST        SPI3_HOST
#define ETH_SPI_SCLK_GPIO   36   /* TODO: set actual GPIO */
#define ETH_SPI_MOSI_GPIO   35   /* TODO: set actual GPIO */
#define ETH_SPI_MISO_GPIO   37   /* TODO: set actual GPIO */
#define ETH_SPI_CS_GPIO     34   /* TODO: set actual GPIO */
#define ETH_INT_GPIO        5    /* TODO: set actual GPIO */
#define ETH_RST_GPIO        (-1) /* Set to GPIO number, or -1 if not wired */
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
