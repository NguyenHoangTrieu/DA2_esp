#ifndef  _ESP_MODEM_COMPAT_H_
#define  _ESP_MODEM_COMPAT_H_

#include "lwip/ip.h"

/**
* @brief ESP Modem Event backward compatible version
*/
typedef enum {
    MODEM_EVENT_PPP_START      = 0x100,
    MODEM_EVENT_PPP_CONNECT    = 0x101,
    MODEM_EVENT_PPP_DISCONNECT = 0x102,
    MODEM_EVENT_PPP_STOP       = 0x103,
    MODEM_EVENT_UNKNOWN        = 0x104,
} esp_modem_compat_event_t;

/**
 * @brief PPPoS Client IP Information backward compatible version
 *
 */
typedef struct {
        ip4_addr_t ip;      /*!< IP Address */
        ip4_addr_t netmask; /*!< Net Mask */
        ip4_addr_t gw;      /*!< Gateway */
        ip4_addr_t ns1;     /*!< Name Server1 */
        ip4_addr_t ns2;     /*!< Name Server2 */
    } ppp_client_ip_info_t;

/**
 * @brief Backward compatible version of esp_modem_set_event_handler()
 */
esp_err_t esp_modem_add_event_handler(modem_dte_t *dte, esp_event_handler_t handler, void *handler_args)  __attribute__ ((deprecated));

/**
 * @brief Backward compatible version of creating esp-netif(PPP) and attaching to esp_modem_start_ppp()
 */
esp_err_t esp_modem_setup_ppp(modem_dte_t *dte) __attribute__ ((deprecated));

/**
 * @brief Backward compatible version of deleting esp-netif and  esp_modem_stop_ppp()
 */
esp_err_t esp_modem_exit_ppp(modem_dte_t *dte) __attribute__ ((deprecated));

#endif