#ifndef _SIM7600_COMM_H_
#define _SIM7600_COMM_H_

#include "esp_modem_uart_dce_service.h"
#include "esp_modem_uart.h"

/**
 * @brief Create and initialize SIM7600 object
 *
 * @param dte Modem DTE object
 * @return modem_dce_t* Modem DCE object
 */
modem_dce_t *sim7600_init(modem_dte_t *dte);

#endif