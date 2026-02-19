#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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
#define UART_BUF_SIZE 8192

/**
 * @brief Mode switch callback type
 * @param mode 0=CONFIG, 1=NORMAL
 */
typedef void (*uart_mode_switch_cb_t)(int mode);

// Function prototypes
void uart_handler_task_start(void);
void uart_handler_task_stop(void);
void uart_handler_register_mode_callback(uart_mode_switch_cb_t callback);

#endif // UART_HANDLER_H
