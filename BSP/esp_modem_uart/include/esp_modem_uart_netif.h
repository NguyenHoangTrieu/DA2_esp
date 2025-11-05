#ifndef ESP_MODEM_UART_NETIF_H
#define ESP_MODEM_UART_NETIF_H

#include "esp_modem_uart.h"
#include "esp_netif.h"
/**
 * @brief Creates handle to esp_modem_uart used as an esp-netif driver
 *
 * @param dte ESP Modem DTE object
 *
 * @return opaque pointer to esp-modem-uart IO driver used to attach to esp-netif
 */
void *esp_modem_uart_netif_setup(modem_dte_t *dte);

/**
 * @brief Destroys the esp-netif driver handle
 *
 * @param h pointer to the esp-netif adapter for esp-modem-uart
 */
void esp_modem_uart_netif_teardown(void *h);

/**
 * @brief Clears default handlers for esp-modem-uart lifecycle
 *
 * @param h pointer to the esp-netif adapter for esp-modem-uart
 */
esp_err_t esp_modem_uart_netif_clear_default_handlers(void);

/**
 * @brief Setups default handlers for esp-modem-uart lifecycle
 *
 * @param h pointer to the esp-netif adapter for esp-modem-uart
 * @param esp_netif pointer corresponding esp-netif instance
 */
esp_err_t esp_modem_uart_netif_set_default_handlers(esp_netif_t * esp_netif);


#endif