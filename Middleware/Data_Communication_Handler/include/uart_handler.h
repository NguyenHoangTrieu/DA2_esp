#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ESP-IDF driver includes
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

// Application includes
#include "config_handler.h"

// Default UART configuration defines
#define DEFAULT_UART_PORT_NUM UART_NUM_0
#define DEFAULT_UART_BAUD_RATE 115200
#define DEFAULT_UART_TX_PIN GPIO_NUM_43
#define DEFAULT_UART_RX_PIN GPIO_NUM_44
#define UART_BUF_SIZE 512

// Function prototypes
void uart_handler_task_start(void);
void uart_handler_task_stop(void);

#endif // UART_HANDLER_H
