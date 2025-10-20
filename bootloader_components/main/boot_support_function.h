#ifndef BOOT_SUPPORT_FUNCTION_H
#define BOOT_SUPPORT_FUNCTION_H

#include <stdbool.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "hal/gpio_ll.h"
#include "esp_rom_gpio.h"
#include "esp_rom_uart.h"

// GPIO pin definitions for slave control
#define SLAVE_BOOT_PIN      39  // GPIO39: Master -> Boot slave
#define SLAVE_RESET_PIN     40  // GPIO40: Master -> RST slave
#define SLAVE_RX_PIN        41  // GPIO41: Master TX -> Slave RX
#define SLAVE_TX_PIN        42  // GPIO42: Master RX <- Slave TX

// Flash bridge protocol commands
#define FLASH_BEGIN_CMD     0x02
#define FLASH_DATA_CMD      0x03
#define FLASH_END_CMD       0x04
#define SYNC_CMD            0x08

// UART configuration for slave communication
#define UART_NUM_SLAVE      1       // Use UART1 for slave communication
#define UART_BAUD_RATE      115200  // Standard bootloader baud rate
#define UART_BUFFER_SIZE    1024

#define UART_NUM_DEBUG 2        // Use UART2 for debug output
#define DEBUG_UART_TX_PIN 17    // GPIO17: UART2 TX for debug
#define DEBUG_UART_RX_PIN 18    // GPIO18: UART2 RX (unused, optional)


// SLIP protocol constants used by ESP32 bootloader
#define SLIP_END            0xC0
#define SLIP_ESC            0xDB
#define SLIP_ESC_END        0xDC
#define SLIP_ESC_ESC        0xDD

#define CONFIG_EXAMPLE_BOOTLOADER_WELCOME_MESSAGE "ESP32 Master-Slave Bootloader"

// Forward declarations
void configure_uart1_for_slave(void);
int uart1_tx_one_char(uint8_t c);
int uart1_rx_one_char(uint8_t *c);

// Debug UART functions
void configure_uart2_debug(void);
void uart2_debug_print(const char* prefix, uint8_t* data, uint32_t len);
void uart2_debug_hex(uint8_t byte);

int select_partition_number(bootloader_state_t *bs);
void init_slave_control_gpios(void);
void enter_slave_bootloader_mode(void);
void reset_slave_normal_mode(void);
bool uart_flash_bridge_mode(void);

// UART helper functions for slave communication
void bootloader_disable_rtc_wdt(void);
void bootloader_feed_wdt(void);
void configure_uart1_for_slave(void);
int uart1_tx_one_char(uint8_t c);
int uart1_rx_one_char(uint8_t *c);
void uart_bridge_passthrough(void);

#endif // BOOT_SUPPORT_FUNCTION_H