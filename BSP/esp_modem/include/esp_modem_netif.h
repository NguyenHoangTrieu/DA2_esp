#ifndef _ESP_MODEM_NETIF_H_
#define _ESP_MODEM_NETIF_H_

#include "esp_modem.h"
#include "esp_netif.h"
/**
 * @brief Creates handle to esp_modem used as an esp-netif driver
 *
 * @param dte ESP Modem DTE object
 *
 * @return opaque pointer to esp-modem IO driver used to attach to esp-netif
 */
void *esp_modem_netif_setup(modem_dte_t *dte);

/**
 * @brief Destroys the esp-netif driver handle
 *
 * @param h pointer to the esp-netif adapter for esp-modem
 */
void esp_modem_netif_teardown(void *h);

/**
 * @brief Clears default handlers for esp-modem lifecycle
 *
 * @param h pointer to the esp-netif adapter for esp-modem
 */
esp_err_t esp_modem_netif_clear_default_handlers(void *h);

/**
 * @brief Setups default handlers for esp-modem lifecycle
 *
 * @param h pointer to the esp-netif adapter for esp-modem
 * @param esp_netif pointer corresponding esp-netif instance
 */
esp_err_t esp_modem_netif_set_default_handlers(void *h, esp_netif_t * esp_netif);


#endif