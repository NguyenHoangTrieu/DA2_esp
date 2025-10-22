#include "boot_support_function.h"

const char *TAG = "boot-support";
static int selected_boot_partition(const bootloader_state_t *bs);

// Get UART hardware instance pointer
static uart_dev_t *uart_ll_get_hw(uint8_t uart_num) {
  switch (uart_num) {
  case 0:
    return &UART0;
  case 1:
    return &UART1;
  case 2:
    return &UART2;
  default:
    return NULL;
  }
}

// Helper: UART0 RX/TX using LL (PC communication)
int uart0_rx_one_char(uint8_t *c) {
  if (uart_ll_get_rxfifo_len(&UART0) == 0) {
    return -1;
  }
  uart_ll_read_rxfifo(&UART0, c, 1);
  return 0;
}

void uart0_tx_one_char(uint8_t c) { uart_ll_write_txfifo(&UART0, &c, 1); }

// Helper: GPIO control for pins > 31
void gpio_set_level_safe(int gpio_num, int level) {
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

void gpio_output_enable_safe(int gpio_num) {
  if (gpio_num < 32)
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1UL << gpio_num));
  else
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, (1UL << (gpio_num - 32)));
}

// Initialize GPIO pins for controlling slave ESP32
void init_slave_control_gpios(void) {
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

  // Configure SLAVE_RX_PIN and SLAVE_TX_PIN for UART
  esp_rom_gpio_pad_select_gpio(SLAVE_RX_PIN);
  esp_rom_gpio_pad_select_gpio(SLAVE_TX_PIN);
}

// Put slave into bootloader mode
void enter_slave_bootloader_mode(void) {
  esp_rom_printf("[%s] Entering slave bootloader mode\n", TAG);

  gpio_set_level_safe(SLAVE_BOOT_PIN, 0);
  esp_rom_delay_us(10000);

  gpio_set_level_safe(SLAVE_RESET_PIN, 0);
  esp_rom_delay_us(100000);

  gpio_set_level_safe(SLAVE_RESET_PIN, 1);
  esp_rom_delay_us(50000);

  gpio_set_level_safe(SLAVE_BOOT_PIN, 1);

  esp_rom_printf("[%s] Slave in bootloader mode\n", TAG);
}

// Reset slave to normal mode
void reset_slave_normal_mode(void) {
  esp_rom_printf("[%s] Resetting slave to normal mode\n", TAG);

  gpio_set_level_safe(SLAVE_BOOT_PIN, 1);
  esp_rom_delay_us(10000);

  gpio_set_level_safe(SLAVE_RESET_PIN, 0);
  esp_rom_delay_us(100000);
  gpio_set_level_safe(SLAVE_RESET_PIN, 1);

  esp_rom_printf("[%s] Slave reset to normal mode\n", TAG);
}

// Configure UART1 for slave communication using LL
void configure_uart1_for_slave(void) {
  periph_ll_enable_clk_clear_rst(PERIPH_UART1_MODULE);

  uart_dev_t *uart = uart_ll_get_hw(UART_NUM_SLAVE);
  if (!uart) {
    esp_rom_printf("[%s] UART1 hardware not found!\n", TAG);
    return;
  }

  const uint32_t sclk_freq = 40000000;
  const uint32_t baud_rate = UART_BAUD_RATE;

  // Reset FIFOs using LL
  uart_ll_txfifo_rst(uart);
  uart_ll_rxfifo_rst(uart);

  // Set baudrate using LL
  uart_ll_set_baudrate(uart, baud_rate, sclk_freq);

  // Configure 8N1 using LL
  uart_ll_set_data_bit_num(uart, UART_DATA_8_BITS);
  uart_ll_set_stop_bits(uart, UART_STOP_BITS_1);
  uart_ll_set_parity(uart, UART_PARITY_DISABLE);

  // Disable flow control using LL
  uart_ll_set_hw_flow_ctrl(uart, UART_HW_FLOWCTRL_DISABLE, 0);

  // Set TX idle using LL
  uart_ll_set_tx_idle_num(uart, 0);

  // Clear and disable interrupts using LL
  uart_ll_clr_intsts_mask(uart, UART_LL_INTR_MASK);
  uart_ll_disable_intr_mask(uart, UART_LL_INTR_MASK);

  // Map GPIOs
  esp_rom_gpio_connect_out_signal(SLAVE_RX_PIN, U1TXD_OUT_IDX, false, false);
  esp_rom_gpio_pad_select_gpio(SLAVE_RX_PIN);

  esp_rom_gpio_connect_in_signal(SLAVE_TX_PIN, U1RXD_IN_IDX, false);
  esp_rom_gpio_pad_select_gpio(SLAVE_TX_PIN);

  esp_rom_printf("[%s] UART1: %d baud, 8N1, TX=GPIO%d, RX=GPIO%d\n", TAG,
                 baud_rate, SLAVE_RX_PIN, SLAVE_TX_PIN);
}

// UART1 TX one byte using LL
void uart1_tx_one_char(uint8_t c) {
  uart_dev_t *uart = uart_ll_get_hw(UART_NUM_SLAVE);
  uart_ll_write_txfifo(uart, &c, 1);
}

// UART1 RX one byte using LL
int uart1_rx_one_char(uint8_t *c) {
  uart_dev_t *uart = uart_ll_get_hw(UART_NUM_SLAVE);
  if (uart_ll_get_rxfifo_len(uart) == 0) {
    return -1;
  }
  uart_ll_read_rxfifo(uart, c, 1);
  return 0;
}

// Configure UART2 for debug using LL
void configure_uart2_debug(void) {

  periph_ll_enable_clk_clear_rst(PERIPH_UART2_MODULE);

  uart_dev_t *uart = uart_ll_get_hw(UART_NUM_DEBUG);
  if (!uart) {
    esp_rom_printf("[%s] UART2 hardware not found!\n", TAG);
    return;
  }

  const uint32_t sclk_freq = 40000000;
  const uint32_t baud_rate = UART_BAUD_RATE;

  // Reset FIFOs
  uart_ll_txfifo_rst(uart);
  uart_ll_rxfifo_rst(uart);

  // Set baudrate
  uart_ll_set_baudrate(uart, baud_rate, sclk_freq);

  // Configure 8N1
  uart_ll_set_data_bit_num(uart, UART_DATA_8_BITS);
  uart_ll_set_stop_bits(uart, UART_STOP_BITS_1);
  uart_ll_set_parity(uart, UART_PARITY_DISABLE);

  // Disable flow control
  uart_ll_set_hw_flow_ctrl(uart, UART_HW_FLOWCTRL_DISABLE, 0);

  // Set TX idle
  uart_ll_set_tx_idle_num(uart, 0);

  // Clear interrupts
  uart_ll_clr_intsts_mask(uart, UART_LL_INTR_MASK);
  uart_ll_disable_intr_mask(uart, UART_LL_INTR_MASK);

  // Map GPIO
  esp_rom_gpio_connect_out_signal(DEBUG_UART_TX_PIN, U2TXD_OUT_IDX, false,
                                  false);
  esp_rom_gpio_pad_select_gpio(DEBUG_UART_TX_PIN);

  esp_rom_printf("[%s] UART2: %d baud, 8N1, TX=GPIO%d\n", TAG, baud_rate,
                 DEBUG_UART_TX_PIN);
}

// UART2 TX one byte using LL
void uart2_tx_one_char(uint8_t c) {
  uart_dev_t *uart = uart_ll_get_hw(UART_NUM_DEBUG);
  uart_ll_write_txfifo(uart, &c, 1);
}

// UART2 print string
void uart2_print_string(const char *str) {
  int data_len = strlen(str);
  uart_ll_write_txfifo(uart_ll_get_hw(UART_NUM_DEBUG), (const uint8_t *)str,
                       data_len);
}

/**
 * @brief UART bridge with timeout and debug logging
 */
void uart_bridge_passthrough(void) {
  uint8_t byte;
  uint32_t pc_to_slave = 0;
  uint32_t slave_to_pc = 0;
  bool flash_in_progress = false;
  uint32_t idle_counter = 0;
  uint32_t wdt_feed_counter = 0;
  uint32_t max_idle = 5000;

  configure_uart1_for_slave();
  esp_rom_printf("[%s] UART bridge active\n", TAG);

  while (1) {
    bool data_activity = false;
    // Forward: PC -> Slave
    if (uart0_rx_one_char(&byte) == 0) {
      uart1_tx_one_char(byte);
      pc_to_slave++;
      data_activity = true;
      if (byte == SLIP_END) {
        flash_in_progress = true;
      }
    }

    // Forward: Slave -> PC
    if (uart1_rx_one_char(&byte) == 0) {
      uart0_tx_one_char(byte);
      slave_to_pc++;
      data_activity = true;
    }

    if (data_activity) {
      idle_counter = 0;
    } else {
      idle_counter++;
      wdt_feed_counter++;
      esp_rom_delay_us(1000);

      if (flash_in_progress && idle_counter > max_idle) {
        esp_rom_printf("[%s] Flash done: %d/%d bytes\n", TAG, pc_to_slave,
                       slave_to_pc);
        reset_slave_normal_mode();
        return;
      }

      if (wdt_feed_counter >= 5000) {
        bootloader_feed_wdt();
        wdt_feed_counter = 0;
      }

      if (idle_counter > 30000) {
        esp_rom_printf("[%s] Timeout\n", TAG);
        reset_slave_normal_mode();
        return;
      }
    }
  }
}

// Select boot partition
int select_partition_number(bootloader_state_t *bs) {
  if (!bootloader_utility_load_partition_table(bs)) {
    ESP_LOGE(TAG, "load partition table error!");
    return INVALID_INDEX;
  }
  return selected_boot_partition(bs);
}

static int selected_boot_partition(const bootloader_state_t *bs) {
  int boot_index = bootloader_utility_get_selected_boot_partition(bs);
  if (boot_index == INVALID_INDEX) {
    return boot_index;
  }
  if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
    bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
    reset_level = true;
#endif
    if (bootloader_common_check_long_hold_gpio_level(
            CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET,
            CONFIG_BOOTLOADER_HOLD_TIME_GPIO, reset_level) == GPIO_LONG_HOLD) {
      ESP_LOGI(TAG, "Factory reset detected");
      bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
      ota_data_erase = true;
#endif
      const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
      ESP_LOGI(TAG, "Erasing: %s", list_erase);
      if (bootloader_common_erase_part_type_data(list_erase, ota_data_erase) ==
          false) {
        ESP_LOGE(TAG, "Not all partitions erased");
      }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
      bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
      return bootloader_utility_get_selected_boot_partition(bs);
    }
#endif
#ifdef CONFIG_BOOTLOADER_APP_TEST
    bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
    app_test_level = true;
#endif
    if (bootloader_common_check_long_hold_gpio_level(
            CONFIG_BOOTLOADER_NUM_PIN_APP_TEST,
            CONFIG_BOOTLOADER_HOLD_TIME_GPIO,
            app_test_level) == GPIO_LONG_HOLD) {
      ESP_LOGI(TAG, "Test firmware boot");
      if (bs->test.offset != 0) {
        boot_index = TEST_APP_INDEX;
        return boot_index;
      } else {
        ESP_LOGE(TAG, "Test firmware not found");
        return INVALID_INDEX;
      }
    }
#endif
  }
  return boot_index;
}

// Configure watchdog
void bootloader_disable_rtc_wdt(void) {
  // RTC WDT
  REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1U);
  REG_WRITE(RTC_CNTL_WDTCONFIG1_REG, 0xFFFFU);
  REG_WRITE(RTC_CNTL_WDTCONFIG2_REG, 0x30000U);
  REG_WRITE(RTC_CNTL_WDTCONFIG3_REG, 0xFFFFU);
  REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, BIT(30) | BIT(29));
  REG_WRITE(RTC_CNTL_WDTFEED_REG, 0xA5U);
  REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0U);

  // Super WDT
  REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0x8F1D312AU);
  uint32_t swd_conf = REG_READ(RTC_CNTL_SWD_CONF_REG);
  swd_conf &= ~0x1FFF;
  swd_conf |= 0x1FFF;
  REG_WRITE(RTC_CNTL_SWD_CONF_REG, swd_conf);
  REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, BIT(1));
  REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0U);

  esp_rom_printf("[boot-support] WDT configured\n");
}

void bootloader_feed_wdt(void) {
  REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1U);
  REG_WRITE(RTC_CNTL_WDTFEED_REG, 0xA5U);
  REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0U);

  REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0x8F1D312AU);
  REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, BIT(1));
  REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0U);
}
