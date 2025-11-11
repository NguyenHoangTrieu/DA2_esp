#ifndef PPP_SERVER_CONFIG_H
#define PPP_SERVER_CONFIG_H

#include "driver/uart.h"
#include "driver/gpio.h"

/* UART Configuration for PPP Server */
#define PPP_UART_PORT           UART_NUM_2
#define PPP_UART_BAUD_RATE      115200
#define PPP_UART_TX_PIN         GPIO_NUM_41
#define PPP_UART_RX_PIN         GPIO_NUM_42

/* UART Buffer Sizes */
#define PPP_UART_BUF_SIZE       (2048)
#define PPP_UART_QUEUE_SIZE     (20)

/* OTA Trigger and Response Commands */
#define PPP_TRIGGER_CMD         "START_OTA\n"
#define PPP_RESPONSE_OK_CMD     "OTA_LAN_OK\n"
#define PPP_RESPONSE_FAIL_CMD   "OTA_LAN_FAIL\n"

/* Command Lengths */
#define PPP_TRIGGER_CMD_LEN     (strlen(PPP_TRIGGER_CMD))
#define PPP_RESPONSE_OK_LEN     (strlen(PPP_RESPONSE_OK_CMD))
#define PPP_RESPONSE_FAIL_LEN   (strlen(PPP_RESPONSE_FAIL_CMD))

/* PPP Server Configuration */
#define PPP_SERVER_TASK_STACK_SIZE  (4 * 1024)
#define PPP_SERVER_TASK_PRIORITY    (5)

#define PPP_SERVER_INTERNET_EPPP_CHANNEL 0

/* Response Buffer Size */
#define PPP_RESPONSE_BUF_SIZE   (128)

/* Default Timeout for LAN Response (in FreeRTOS ticks) */
#define PPP_DEFAULT_RESPONSE_TIMEOUT_MS  (120000)  // 2 minutes

#endif /* PPP_SERVER_CONFIG_H */
