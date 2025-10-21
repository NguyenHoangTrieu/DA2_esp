#ifndef BOOT_SUPPORT_FUNCTION_H
#define BOOT_SUPPORT_FUNCTION_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"         // HAL UART
#include "soc/uart_struct.h"     // UART hardware struct
#include "soc/periph_defs.h"
#include "esp_private/periph_ctrl.h"
#include "hal/clk_gate_ll.h"
#include "esp_rom_gpio.h"
#include "esp_rom_uart.h"

// GPIO pin definitions for slave control
#define SLAVE_BOOT_PIN 39
#define SLAVE_RESET_PIN 40
#define SLAVE_RX_PIN 41
#define SLAVE_TX_PIN 42

// UART configuration
#define UART_NUM_SLAVE 1
#define UART_NUM_DEBUG 2
#define UART_BAUD_RATE 115200
#define UART_BUFFER_SIZE 1024

// Debug UART2 pins
#define DEBUG_UART_TX_PIN 17
#define DEBUG_UART_RX_PIN 18

// SLIP protocol constants
#define SLIP_END 0xC0

#define CONFIG_EXAMPLE_BOOTLOADER_WELCOME_MESSAGE "ESP32 Master-Slave Bootloader"

// Bit macro
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

// Function declarations
void init_slave_control_gpios(void);
void enter_slave_bootloader_mode(void);
void reset_slave_normal_mode(void);
void uart_bridge_passthrough(void);

// UART configuration functions
void configure_uart1_for_slave(void);
void configure_uart2_debug(void);
void uart1_tx_one_char(uint8_t c);
int uart1_rx_one_char(uint8_t *c);

// Boot partition selection
int select_partition_number(bootloader_state_t *bs);

// Watchdog functions
void bootloader_disable_rtc_wdt(void);
void bootloader_feed_wdt(void);

#endif // BOOT_SUPPORT_FUNCTION_H
