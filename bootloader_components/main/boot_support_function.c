#include "boot_support_function.h"

const char* TAG = "boot-support";
static int selected_boot_partition(const bootloader_state_t *bs);

// Helper macro for GPIO > 31
static inline void gpio_set_level_safe(int gpio_num, int level) {
    if (gpio_num < 32) {
        if (level)
            REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << gpio_num));
        else
            REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << gpio_num));
    } else {
        if (level)
            REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (gpio_num - 32)));
        else
            REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (gpio_num - 32)));
    }
}

static inline void gpio_output_enable_safe(int gpio_num) {
    if (gpio_num < 32)
        REG_WRITE(GPIO_ENABLE_W1TS_REG, (1UL << gpio_num));
    else
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, (1UL << (gpio_num - 32)));
}

// Initialize GPIO pins for controlling slave ESP32
void init_slave_control_gpios(void)
{
    // Configure SLAVE_BOOT_PIN as output (default HIGH)
    esp_rom_gpio_pad_select_gpio(SLAVE_BOOT_PIN);
    esp_rom_gpio_pad_set_drv(SLAVE_BOOT_PIN, 2);
    gpio_output_enable_safe(SLAVE_BOOT_PIN);
    gpio_set_level_safe(SLAVE_BOOT_PIN, 1);

    // Configure SLAVE_RESET_PIN as output (default HIGH)
    esp_rom_gpio_pad_select_gpio(SLAVE_RESET_PIN);
    esp_rom_gpio_pad_set_drv(SLAVE_RESET_PIN, 2);
    gpio_output_enable_safe(SLAVE_RESET_PIN);
    gpio_set_level_safe(SLAVE_RESET_PIN, 1);

    // Configure SLAVE_RX_PIN and SLAVE_TX_PIN for UART functionality
    // These will be configured by UART initialization
    esp_rom_gpio_pad_select_gpio(SLAVE_RX_PIN);
    esp_rom_gpio_pad_select_gpio(SLAVE_TX_PIN);
}

// Put slave into bootloader download mode
void enter_slave_bootloader_mode(void)
{
    esp_rom_printf("[%s] Entering slave bootloader mode\n", TAG);

    // Step 1: Pull BOOT pin LOW (GPIO0 on slave)
    gpio_set_level_safe(SLAVE_BOOT_PIN, 0);
    esp_rom_delay_us(10000); // 10ms delay

    // Step 2: Pull RESET pin LOW to reset slave
    gpio_set_level_safe(SLAVE_RESET_PIN, 0);
    esp_rom_delay_us(100000); // 100ms hold time

    // Step 3: Release RESET pin (pull HIGH)
    gpio_set_level_safe(SLAVE_RESET_PIN, 1);
    esp_rom_delay_us(50000); // 50ms delay for slave to enter bootloader

    // Step 4: Release BOOT pin (pull HIGH)
    gpio_set_level_safe(SLAVE_BOOT_PIN, 1);

    esp_rom_printf("[%s] Slave should be in bootloader mode now\n", TAG);
}

// Reset slave to normal boot mode
void reset_slave_normal_mode(void)
{
    esp_rom_printf("[%s] Resetting slave to normal boot mode\n", TAG);

    // Ensure BOOT pin is HIGH (normal boot)
    gpio_set_level_safe(SLAVE_BOOT_PIN, 1);
    esp_rom_delay_us(10000);

    // Toggle RESET pin
    gpio_set_level_safe(SLAVE_RESET_PIN, 0);
    esp_rom_delay_us(100000); // 100ms
    gpio_set_level_safe(SLAVE_RESET_PIN, 1);

    esp_rom_printf("[%s] Slave reset to normal mode\n", TAG);
}

// UART flash bridge mode - acts as transparent bridge between PC and slave
bool uart_flash_bridge_mode(void)
{
    uint8_t uart_buffer[UART_BUFFER_SIZE];
    bool flash_end_detected = false;
    uint32_t timeout_counter = 0;
    uint32_t max_timeout = 30000; // 30 seconds timeout

    esp_rom_printf("[%s] Starting UART bridge mode\n", TAG);

    // Configure UART1 for communication with slave
    // Use UART0 for PC communication (already configured by ROM bootloader)
    esp_rom_uart_tx_wait_idle(0);

    // Main bridge loop
    while (!flash_end_detected && timeout_counter < max_timeout) {
        // Check for data from PC (UART0) to forward to slave
        int pc_data_len = 0;
        while (esp_rom_uart_rx_one_char((uint8_t*)&uart_buffer[pc_data_len]) == 0) {
            pc_data_len++;
            if (pc_data_len >= UART_BUFFER_SIZE) {
                break;
            }
            esp_rom_delay_us(100);
        }

        // Forward data from PC to slave if any received
        if (pc_data_len > 0) {
            for (int i = 0; i < pc_data_len; i++) {
                if (uart_buffer[i] == FLASH_END_CMD) {
                    flash_end_detected = true;
                    esp_rom_printf("[%s] Flash end command detected\n", TAG);
                }
            }
            uart_bridge_passthrough();
            timeout_counter = 0;
        }

        // Increment timeout counter
        timeout_counter++;
        esp_rom_delay_us(1000); // 1ms delay
    }

    if (flash_end_detected) {
        esp_rom_printf("[%s] Bridge mode completed successfully\n", TAG);
        return true;
    } else {
        esp_rom_printf("[%s] Bridge mode timeout\n", TAG);
        return false;
    }
}

// Configure UART1 peripheral for slave communication
void configure_uart1_for_slave(void)
{
    // APB clock is 80MHz in bootloader
    const uint32_t apb_clk = 80000000;
    const uint32_t baud_rate = UART_BAUD_RATE; // Assuming UART_BAUD_RATE is 115200

    // Correct baud rate calculation
    uint32_t clkdiv_integer = apb_clk / baud_rate;
    uint32_t clkdiv_frag = ((apb_clk % baud_rate) * 16) / baud_rate;
    uint32_t divider = (clkdiv_integer << 4) | (clkdiv_frag & 0xF);

    // Reset FIFOs
    REG_SET_BIT(UART_CONF0_REG(UART_NUM_SLAVE), UART_TXFIFO_RST | UART_RXFIFO_RST);
    REG_CLR_BIT(UART_CONF0_REG(UART_NUM_SLAVE), UART_TXFIFO_RST | UART_RXFIFO_RST);

    // Configure 8N1
    REG_WRITE(UART_CONF0_REG(UART_NUM_SLAVE),
              (0x3 << UART_BIT_NUM_S) |      // 8 data bits
              (0x1 << UART_STOP_BIT_NUM_S) | // 1 stop bit
              (0x0 << UART_PARITY_EN_S));    // No parity

    // Set the calculated baud rate divider
    REG_WRITE(UART_CLKDIV_REG(UART_NUM_SLAVE), divider);

    // Map GPIOs using GPIO Matrix
    // Master TX (SLAVE_RX_PIN) -> Slave RX
    esp_rom_gpio_connect_out_signal(SLAVE_RX_PIN, U1TXD_OUT_IDX, false, false);
    esp_rom_gpio_pad_select_gpio(SLAVE_RX_PIN);
    
    // Master RX (SLAVE_TX_PIN) <- Slave TX
    esp_rom_gpio_connect_in_signal(SLAVE_TX_PIN, U1RXD_IN_IDX, false);
    esp_rom_gpio_pad_select_gpio(SLAVE_TX_PIN);
    esp_rom_gpio_pad_pullup_only(SLAVE_TX_PIN); // Pull-up on RX is good practice

    esp_rom_printf("[%s] UART1 configured: %d 8N1, TX=GPIO%d, RX=GPIO%d\n",
                   TAG, baud_rate, SLAVE_RX_PIN, SLAVE_TX_PIN);
}

int uart1_tx_one_char(uint8_t c)
{
    uint32_t timeout = 0;
    while (REG_GET_FIELD(UART_STATUS_REG(UART_NUM_SLAVE), UART_TXFIFO_CNT) >= 127) {
        if (timeout++ > 100000) {
            return -1;
        }
        esp_rom_delay_us(1);
    }
    REG_WRITE(UART_FIFO_REG(UART_NUM_SLAVE), c);
    return 0;
}

int uart1_rx_one_char(uint8_t *c)
{
    if (REG_GET_FIELD(UART_STATUS_REG(UART_NUM_SLAVE), UART_RXFIFO_CNT) == 0) {
        return -1;
    }
    *c = REG_READ(UART_FIFO_REG(UART_NUM_SLAVE)) & 0xFF;
    return 0;
}

// Configure UART2 for debug output
void configure_uart2_debug(void)
{
    const uint32_t apb_clk = 80000000;  // 80MHz APB clock
    const uint32_t baud_rate = UART_BAUD_RATE;

    // Correct baud rate calculation
    uint32_t clkdiv_integer = apb_clk / baud_rate;
    uint32_t clkdiv_frag = ((apb_clk % baud_rate) * 16) / baud_rate;
    uint32_t divider = (clkdiv_integer << 4) | (clkdiv_frag & 0xF);
    
    // Reset UART2 FIFOs
    REG_SET_BIT(UART_CONF0_REG(UART_NUM_DEBUG), UART_TXFIFO_RST | UART_RXFIFO_RST);
    REG_CLR_BIT(UART_CONF0_REG(UART_NUM_DEBUG), UART_TXFIFO_RST | UART_RXFIFO_RST);
    
    // Configure: 8N1 (8 data bits, no parity, 1 stop bit)
    REG_WRITE(UART_CONF0_REG(UART_NUM_DEBUG),
              (0x3 << UART_BIT_NUM_S) |     // 8 data bits
              (0x1 << UART_STOP_BIT_NUM_S) | // 1 stop bit
              (0x0 << UART_PARITY_EN_S));    // No parity
    
    // Set baud rate
    REG_WRITE(UART_CLKDIV_REG(UART_NUM_DEBUG), divider);
    
    // Map UART2 TX to GPIO (RX not needed for debug output)
    esp_rom_gpio_connect_out_signal(DEBUG_UART_TX_PIN, U2TXD_OUT_IDX, false, false);
    esp_rom_gpio_pad_select_gpio(DEBUG_UART_TX_PIN);
    // esp_rom_gpio_pad_pullup_only(DEBUG_UART_TX_PIN); // Not necessary for a TX pin

    esp_rom_printf("[%s] UART2 debug configured: %d 8N1, TX=GPIO%d\n",
                   TAG, baud_rate, DEBUG_UART_TX_PIN);
}

// Transmit one character over UART2 debug
static int uart2_tx_one_char(uint8_t c)
{
    uint32_t timeout = 0;
    
    // Wait for FIFO space
    while (REG_GET_FIELD(UART_STATUS_REG(UART_NUM_DEBUG), UART_TXFIFO_CNT) >= 127) {
        if (timeout++ > 10000) {
            return -1;  // Timeout
        }
        esp_rom_delay_us(1);
    }
    
    REG_WRITE(UART_FIFO_REG(UART_NUM_DEBUG), c);
    return 0;
}

// Helper function to print string to UART2
static void uart2_print_string(const char* str)
{
    while (*str) {
        uart2_tx_one_char(*str++);
    }
}

// Print a single byte as two hex characters to UART2
void uart2_debug_hex(uint8_t byte)
{
    const char hex[] = "0123456789ABCDEF";
    uart2_tx_one_char(hex[(byte >> 4) & 0xF]);
    uart2_tx_one_char(hex[byte & 0xF]);
    uart2_tx_one_char(' ');
}

// Print a buffer in hex format to UART2 with a prefix
void uart2_debug_print(const char* prefix, uint8_t* data, uint32_t len)
{
    if (len == 0) return;
    
    uart2_print_string(prefix);
    uart2_print_string(" ");
    
    for (uint32_t i = 0; i < len && i < 16; i++) {
        uart2_debug_hex(data[i]);
    }
    
    if (len > 16) {
        uart2_print_string("... (");
        // Simple decimal print (no sprintf available)
        if (len >= 100) uart2_tx_one_char('0' + (len / 100));
        if (len >= 10) uart2_tx_one_char('0' + ((len / 10) % 10));
        uart2_tx_one_char('0' + (len % 10));
        uart2_print_string(" bytes)");
    }
    
    uart2_print_string("\r\n");
}

void uart_bridge_passthrough(void)
{
    uint8_t byte;
    uint8_t debug_buffer[32];  // Buffer for UART2 debug
    uint32_t debug_buf_idx = 0;
    uint32_t bytes_forwarded = 0;
    uint32_t pc_to_slave = 0;
    uint32_t slave_to_pc = 0;
    bool flash_in_progress = false;
    uint32_t idle_counter = 0;
    uint32_t wdt_feed_counter = 0;
    uint32_t max_idle = 5000;
    
    configure_uart1_for_slave();
    configure_uart2_debug();  // ← Initialize UART2 debug
    
    esp_rom_printf("[%s] UART bridge active - forwarding data...\n", TAG);
    uart2_print_string("[DEBUG] Bridge started, monitor on UART2\r\n");
    
    while (1) {
        bool data_activity = false;
        
        // Forward data from PC (UART0) to slave (UART1)
        debug_buf_idx = 0;
        while (esp_rom_uart_rx_one_char(&byte) == 0) {
            // Store for debug before forwarding
            if (debug_buf_idx < sizeof(debug_buffer)) {
                debug_buffer[debug_buf_idx++] = byte;
            }
            
            if (uart1_tx_one_char(byte) == 0) {
                pc_to_slave++;
                data_activity = true;
                if (byte == SLIP_END) {
                    flash_in_progress = true;
                }
            } else {
                esp_rom_printf("[%s] TX to slave timeout!\n", TAG);
                break;
            }
        }
        
        // Debug print PC -> Slave data
        if (debug_buf_idx > 0) {
            uart2_debug_print("[PC->SLV]", debug_buffer, debug_buf_idx);
        }
        
        // Forward data from slave (UART1) to PC (UART0)
        debug_buf_idx = 0;
        while (uart1_rx_one_char(&byte) == 0) {
            // Store for debug before forwarding
            if (debug_buf_idx < sizeof(debug_buffer)) {
                debug_buffer[debug_buf_idx++] = byte;
            }
            
            esp_rom_uart_tx_one_char(byte);
            slave_to_pc++;
            data_activity = true;
        }
        
        // Debug print Slave -> PC data
        if (debug_buf_idx > 0) {
            uart2_debug_print("[SLV->PC]", debug_buffer, debug_buf_idx);
        }
        
        if (data_activity) {
            idle_counter = 0;
            if ((pc_to_slave + slave_to_pc - bytes_forwarded) >= 1000) {
                esp_rom_printf("[%s] Progress: PC->Slave=%d, Slave->PC=%d bytes\n",
                               TAG, pc_to_slave, slave_to_pc);
                bytes_forwarded = pc_to_slave + slave_to_pc;
            }
        } else {
            idle_counter++;
            wdt_feed_counter++;
            esp_rom_delay_us(1000);
            
            if (flash_in_progress && idle_counter > max_idle) {
                esp_rom_printf("[%s] Flash operation completed (idle timeout)\n", TAG);
                esp_rom_printf("[%s] Total: PC->Slave=%d, Slave->PC=%d bytes\n",
                               TAG, pc_to_slave, slave_to_pc);
                uart2_print_string("[DEBUG] Bridge ended\r\n");
                return;
            }
            
            if (wdt_feed_counter >= 5000) {  // Feed WDT every 5s
                bootloader_feed_wdt();
                wdt_feed_counter = 0;
            }
            
            if (idle_counter > 10000) {
                esp_rom_printf("[%s] Maximum bridge timeout reached\n", TAG);
                uart2_print_string("[DEBUG] Timeout - bridge ended\r\n");
                return;
            }
        }
    }
}

// Select the number of boot partition
int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return selected_boot_partition(bs);
}

/*
 * Selects a boot partition.
 * The conditions for switching to another firmware are checked.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index; // Unrecoverable failure (not due to corrupt ota data or bad partition contents)
    }
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        // Factory firmware.
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
        bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
        reset_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, reset_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a condition of the factory reset");
            bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
            ota_data_erase = true;
#endif
            const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
            ESP_LOGI(TAG, "Data partitions to erase: %s", list_erase);
            if (bootloader_common_erase_part_type_data(list_erase, ota_data_erase) == false) {
                ESP_LOGE(TAG, "Not all partitions were erased");
            }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
            bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
            return bootloader_utility_get_selected_boot_partition(bs);
        }
#endif // CONFIG_BOOTLOADER_FACTORY_RESET
        // TEST firmware.
#ifdef CONFIG_BOOTLOADER_APP_TEST
        bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
        app_test_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_APP_TEST, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, app_test_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a boot condition of the test firmware");
            if (bs->test.offset != 0) {
                boot_index = TEST_APP_INDEX;
                return boot_index;
            } else {
                ESP_LOGE(TAG, "Test firmware is not found in partition table");
                return INVALID_INDEX;
            }
        }
#endif // CONFIG_BOOTLOADER_APP_TEST
        // Customer implementation.
        // if (gpio_pin_1 == true && ...){
        //     boot_index = required_boot_partition;
        // } ...
    }
    return boot_index;
}

// Change RTC and Super Watchdog timeout to 30 seconds
void bootloader_disable_rtc_wdt(void)
{
    // ===== Configure RTC Watchdog for 30s timeout =====
    
    // Unlock write protection
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1U);
    
    REG_WRITE(RTC_CNTL_WDTCONFIG1_REG, 0xFFFFU);   // Stage 1: ~22s
    REG_WRITE(RTC_CNTL_WDTCONFIG2_REG, 0x30000U);  // Stage 2: +8s = 30s total
    REG_WRITE(RTC_CNTL_WDTCONFIG3_REG, 0xFFFFU);   // Stage 3: backup
    
    // Clear response modes (no reset/interrupt)
    REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, BIT(30) | BIT(29));
    
    // Feed to reset counter
    REG_WRITE(RTC_CNTL_WDTFEED_REG, 0xA5U);
    
    // Lock protection
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0U);
    
    esp_rom_printf("[boot-support] RTC WDT configured\n");
    
    // ===== Configure Super Watchdog for 30s timeout =====
    
    // Unlock SWD protection
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0x8F1D312AU);
    
    uint32_t swd_conf = REG_READ(RTC_CNTL_SWD_CONF_REG);
    swd_conf &= ~0x1FFF;         // Clear timeout bits (12:0)
    swd_conf |= 0x1FFF;          // Set to max 0x1FFF for longest timeout (~30-40s)
    REG_WRITE(RTC_CNTL_SWD_CONF_REG, swd_conf);
    
    // Feed SWD counter
    REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, BIT(1));
    
    // Lock SWD protection
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0U);
    
    esp_rom_printf("[boot-support] Super WDT configured\n");
}

void bootloader_feed_wdt(void)
{
    // Feed RTC WDT
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1U);
    REG_WRITE(RTC_CNTL_WDTFEED_REG, 0xA5U);
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0U);
    
    // Feed Super WDT (write 1 to bit 1)
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0x8F1D312AU);
    REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, BIT(1));
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0U);
}