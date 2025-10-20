/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "boot_support_function.h"

static const char* TAG = "boot start";

#define CONFIG_EXAMPLE_BOOTLOADER_WELCOME_MESSAGE "ESP32 Master-Slave Bootloader"

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    // 2. Initialize GPIO pins for slave control
    init_slave_control_gpios();
    
    // 3. Enter flash bridge mode for slave programming
    esp_rom_printf("[%s] Entering flash bridge mode for slave programming\n", TAG);
    
    // Put slave into bootloader mode
    enter_slave_bootloader_mode();
    
    // Start UART bridge passthrough for slave flashing
    if (uart_flash_bridge_mode()) {
        esp_rom_printf("[%s] Slave flash completed successfully\n", TAG);
        
        // Reset slave to normal boot mode
        reset_slave_normal_mode();
    } else {
        esp_rom_printf("[%s] Slave flash failed or timeout\n", TAG);
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // If this boot is a wake up from the deep sleep then go to the short way,
    // try to load the application which worked before deep sleep.
    // It skips a lot of checks due to it was done before (while first boot).
    bootloader_utility_load_boot_image_from_deep_sleep();
    // If it is not successful try to load an application as usual.
#endif

    // 4. Now proceed with master flash/boot
    esp_rom_printf("[%s] Starting master boot sequence\n", TAG);
    
    // Select the number of boot partition for master
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 5. Print a custom message
    esp_rom_printf("[%s] %s\n", TAG, CONFIG_EXAMPLE_BOOTLOADER_WELCOME_MESSAGE);

    // 6. Load the app image for booting master
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
