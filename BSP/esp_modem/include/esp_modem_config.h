#ifndef ESP_MODEM_CONFIG_H
#define ESP_MODEM_CONFIG_H
#include "driver/uart.h"

/**
 * @brief Access Point Name (APN) for mobile network
 * 
 * Logical name used to select the GGSN or external packet data network.
 * Default: "CMNET" (China Mobile)
 * 
 * Common APNs:
 * - China Mobile: "CMNET" or "CMWAP"
 * - Viettel (Vietnam): "v-internet" or "e-connect"
 * - Vinaphone (Vietnam): "m3-world"
 * - Mobifone (Vietnam): "m-wap"
 */
#ifndef CONFIG_COMPONENT_MODEM_APN
#define CONFIG_COMPONENT_MODEM_APN "m3-world"
#endif

/**
 * @brief Personal Identification Number (PIN) for SIM card
 * 
 * PIN code used to unlock the SIM card. Leave empty if no PIN required.
 * Default: "" (empty string - no PIN)
 */
#ifndef CONFIG_COMPONENT_MODEM_PIN
#define CONFIG_COMPONENT_MODEM_PIN ""
#endif

/* Helper macros for APN and PIN access */
#define MODEM_APN_DEFAULT       CONFIG_COMPONENT_MODEM_APN
#define MODEM_PIN_DEFAULT       CONFIG_COMPONENT_MODEM_PIN
#define MODEM_HAS_PIN()         (CONFIG_COMPONENT_MODEM_PIN[0] != '\0')

/* UART Configuration */

#ifndef ESP_MODEM_CONFIG_UART_PORT_NUM
#define ESP_MODEM_CONFIG_UART_PORT_NUM        UART_NUM_2
#endif

#ifndef ESP_MODEM_CONFIG_UART_BAUD_RATE
#define ESP_MODEM_CONFIG_UART_BAUD_RATE       115200
#endif

#ifndef ESP_MODEM_CONFIG_UART_DATA_BITS
#define ESP_MODEM_CONFIG_UART_DATA_BITS       UART_DATA_8_BITS  /* UART_DATA_8_BITS */
#endif

#ifndef ESP_MODEM_CONFIG_UART_STOP_BITS
#define ESP_MODEM_CONFIG_UART_STOP_BITS       UART_STOP_BITS_1  /* UART_STOP_BITS_1 */
#endif

#ifndef ESP_MODEM_CONFIG_UART_PARITY
#define ESP_MODEM_CONFIG_UART_PARITY          UART_PARITY_DISABLE  /* UART_PARITY_DISABLE */
#endif

#ifndef ESP_MODEM_CONFIG_UART_FLOW_CONTROL
#define ESP_MODEM_CONFIG_UART_FLOW_CONTROL    MODEM_FLOW_CONTROL_NONE
#endif


#ifndef ESP_MODEM_CONFIG_UART_TX_PIN
#define ESP_MODEM_CONFIG_UART_TX_PIN          20
#endif

#ifndef ESP_MODEM_CONFIG_UART_RX_PIN
#define ESP_MODEM_CONFIG_UART_RX_PIN          21
#endif

#ifndef ESP_MODEM_CONFIG_UART_RTS_PIN
#define ESP_MODEM_CONFIG_UART_RTS_PIN         27
#endif

#ifndef ESP_MODEM_CONFIG_UART_CTS_PIN
#define ESP_MODEM_CONFIG_UART_CTS_PIN         23
#endif

#ifndef ESP_MODEM_CONFIG_UART_EVENT_TASK_STACK_SIZE
#define ESP_MODEM_CONFIG_UART_EVENT_TASK_STACK_SIZE   2048
#endif

#ifndef ESP_MODEM_CONFIG_UART_EVENT_TASK_PRIORITY
#define ESP_MODEM_CONFIG_UART_EVENT_TASK_PRIORITY     5
#endif

#ifndef ESP_MODEM_CONFIG_UART_EVENT_QUEUE_SIZE
#define ESP_MODEM_CONFIG_UART_EVENT_QUEUE_SIZE        30
#endif

#ifndef ESP_MODEM_CONFIG_UART_PATTERN_QUEUE_SIZE
#define ESP_MODEM_CONFIG_UART_PATTERN_QUEUE_SIZE      20
#endif

#ifndef ESP_MODEM_CONFIG_UART_TX_BUFFER_SIZE
#define ESP_MODEM_CONFIG_UART_TX_BUFFER_SIZE          1024
#endif

#ifndef ESP_MODEM_CONFIG_UART_RX_BUFFER_SIZE
#define ESP_MODEM_CONFIG_UART_RX_BUFFER_SIZE          16384
#endif

#endif /* ESP_MODEM_CONFIG_H */
