#ifndef PPP_SERVER_H
#define PPP_SERVER_H

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Initialize the PPP server
 * 
 * This function initializes the UART, starts the PPP server service,
 * and enables NAPT on the provided (and already-connected) wifi_netif handle.
 * 
 * @param wifi_netif Pointer to the already-initialized and connected Wi-Fi netif
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t ppp_server_init(esp_netif_t *wifi_netif);

/**
 * @brief Deinitialize the PPP server
 * 
 * Stops the PPP server and cleans up resources.
 * 
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t ppp_server_deinit(void);

#endif /* PPP_SERVER_H */
