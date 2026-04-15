/*
 * DA2_esp — ESP32-S3 WAN Gateway Main Application
 * Firmware v1.0.1
 */

#include "DA2_esp.h"

/* ===== Module Tag ===================================================== */
static const char *TAG = "MAIN";

/* ===== Task Handle ==================================================== */
TaskHandle_t main_task_handle = NULL;

/* ===== GPIO =========================================================== */
#define GPIO3_POWER_RGB_CTRL GPIO_NUM_3  /* Toggle power rails + RGB LED */
#define UART_SWITCH_SEL_GPIO                                                   \
  GPIO_NUM_46 // UART_SEL: LOW=LAN MCU (SW1), HIGH=HMI LCD (SW2)
#define ADAPTER_RESET_GPIO GPIO_NUM_48 /* Output: reset adapter IOX at boot */
/* ===== Task Notification Values ======================================= */
/* Each must be a unique value — eSetValueWithOverwrite overwrites        */
#define NOTIFY_BUTTON_PRESS 1     /* GPIO0: toggle CONFIG/NORMAL       */
#define NOTIFY_GPIO3_PRESS 2      /* GPIO3: toggle power/RGB            */
#define NOTIFY_UART_MODE_SWITCH 3 /* UART callback: CONFIG or NORMAL    */

/* ===== Application State ============================================== */
typedef enum {
  APP_MODE_NORMAL = 0,
  APP_MODE_CONFIG,
} app_mode_t;

static app_mode_t current_mode = APP_MODE_NORMAL;
static int requested_mode = -1; /* set by uart_mode_switch_callback */
static uint32_t last_isr_tick = 0;
static uint32_t last_gpio3_tick = 0;
static bool battery_source_enabled = true; /* GPIO3: battery FET state */

/**
 * @brief GPIO0 ISR - Button press
 */
static void gpio0_isr_handler(void *arg) {
  uint32_t now = xTaskGetTickCountFromISR();
  if ((now - last_isr_tick) >= pdMS_TO_TICKS(500)) {
    last_isr_tick = now;
    BaseType_t xTaskWoken = pdFALSE;
    if (main_task_handle) {
      xTaskNotifyFromISR(main_task_handle, NOTIFY_BUTTON_PRESS,
                         eSetValueWithOverwrite, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

/**
 * @brief GPIO3 ISR - Toggle battery source enable/disable
 */
static void gpio3_isr_handler(void *arg) {
  uint32_t now = xTaskGetTickCountFromISR();
  if ((now - last_gpio3_tick) >= pdMS_TO_TICKS(300)) {
    last_gpio3_tick = now;
    BaseType_t xTaskWoken = pdFALSE;
    if (main_task_handle) {
      xTaskNotifyFromISR(main_task_handle, NOTIFY_GPIO3_PRESS,
                         eSetValueWithOverwrite, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

void setup_gpio0_interrupt(void) {
  gpio_config_t gpio0_cfg = {.pin_bit_mask = BIT64(0),
                              .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE,
                              .intr_type = GPIO_INTR_NEGEDGE};

  ESP_ERROR_CHECK(gpio_config(&gpio0_cfg));
  ESP_ERROR_CHECK(gpio_isr_handler_add(0, gpio0_isr_handler, NULL));

  ESP_LOGI(TAG, "GPIO0 button interrupt configured");
}

void setup_gpio3_interrupt(void) {
  gpio_config_t gpio3_cfg = {.pin_bit_mask = BIT64(GPIO3_POWER_RGB_CTRL),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .intr_type = GPIO_INTR_NEGEDGE};

  ESP_ERROR_CHECK(gpio_config(&gpio3_cfg));
  ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO3_POWER_RGB_CTRL, gpio3_isr_handler, NULL));

  ESP_LOGI(TAG, "GPIO3 power/RGB interrupt configured");
}

/* =====================================================================
 *  UART Switch (FSUSB42UMX-TP, Sel = GPIO46)
 *  LOW  (Sel=0) → UART_SW1 → LAN MCU  (normal / PPP / OTA path)
 *  HIGH (Sel=1) → UART_SW2 → HMI LCD  (display config mode)
 * ===================================================================== */

void uart_switch_init(void) {
  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << UART_SWITCH_SEL_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&cfg));
  gpio_set_level(UART_SWITCH_SEL_GPIO, 0); // Default: route to LAN MCU
  ESP_LOGI(TAG, "UART switch initialized (GPIO%d) -> LAN MCU",
           UART_SWITCH_SEL_GPIO);
}

void uart_switch_route_to_lan_mcu(void) {
  gpio_set_level(UART_SWITCH_SEL_GPIO, 0);
  ESP_LOGI(TAG, "UART switch -> LAN MCU");
}

void uart_switch_route_to_hmi(void) {
  gpio_set_level(UART_SWITCH_SEL_GPIO, 1);
  ESP_LOGI(TAG, "UART switch -> HMI LCD");
}

/* =====================================================================
 *  Server Task Helpers
 * ===================================================================== */

void server_connect_stop(config_server_type_t type) {
  switch (type) {
  case CONFIG_SERVERTYPE_MQTT:
    mqtt_handler_task_stop();
    break;
  case CONFIG_SERVERTYPE_HTTP:
    http_handler_task_stop();
    break;
  case CONFIG_SERVERTYPE_COAP:
    coap_handler_task_stop();
    break;
  default:
    ESP_LOGW(TAG, "server_connect_stop: unhandled type %d", type);
    break;
  }
}

void server_connect_start(config_server_type_t type) {
  switch (type) {
  case CONFIG_SERVERTYPE_MQTT:
    mqtt_handler_task_start();
    break;
  case CONFIG_SERVERTYPE_HTTP:
    http_handler_task_start();
    break;
  case CONFIG_SERVERTYPE_COAP:
    coap_handler_task_start();
    break;
  default:
    ESP_LOGW(TAG, "server_connect_start: unknown type %d, defaulting to MQTT",
             type);
    mqtt_handler_task_start();
    break;
  }
}

/* =====================================================================
 *  Internet Task Helpers
 * ===================================================================== */

static void internet_connect_start(config_internet_type_t type) {
  switch (type) {
  case CONFIG_INTERNET_LTE:
    ESP_LOGI(TAG, "Starting LTE connection");
    lte_connect_task_start();
    break;
  case CONFIG_INTERNET_WIFI:
    ESP_LOGI(TAG, "Starting WiFi connection");
    wifi_connect_task_start();
    break;
  case CONFIG_INTERNET_ETHERNET:
    ESP_LOGI(TAG, "Starting Ethernet (W5500) connection");
    eth_connect_task_start();
    break;
  default:
    ESP_LOGW(TAG, "internet_connect_start: unhandled type %d", type);
    break;
  }
}

/* =====================================================================
 *  UART Mode Callback
 * ===================================================================== */

/**
 * @brief Called from uart_handler task when CONFIG/NORMAL command received.
 * @param mode  0 = CONFIG, 1 = NORMAL
 */
static void uart_mode_switch_callback(int mode) {
  requested_mode = mode;
  if (main_task_handle) {
    xTaskNotify(main_task_handle, NOTIFY_UART_MODE_SWITCH,
                eSetValueWithOverwrite);
  }
}

/* =====================================================================
 *  Mode Transitions
 * ===================================================================== */

/**
 * @brief Enter CONFIG mode.
 *
 * If LTE is active, the LTE task is stopped first and the USB bus is
 * released before the switch is flipped to HOST, preventing enumeration
 * conflicts on the shared USB line.
 */
static void switch_to_config_mode(config_internet_type_t *internet_type) {
  if (current_mode == APP_MODE_CONFIG) {
    ESP_LOGW(TAG, "Already in CONFIG mode");
    return;
  }

  ESP_LOGI(TAG, "==> Entering CONFIG mode");

  /* When LTE is active the modem owns the USB bus — stop it first */
  if (*internet_type == CONFIG_INTERNET_LTE) {
    ESP_LOGI(TAG, "Stopping LTE task before USB switch");
    lte_connect_task_stop();
    vTaskDelay(pdMS_TO_TICKS(10000)); /* wait for modem to release USB */
  }

  /* Stop Ethernet driver before entering CONFIG mode */
  if (*internet_type == CONFIG_INTERNET_ETHERNET) {
    ESP_LOGI(TAG, "Stopping Ethernet task before CONFIG mode");
    eth_connect_task_stop();
  }

  vTaskDelay(pdMS_TO_TICKS(100));
  jtag_task_start();

  /* Start WiFi AP + web portal + captive DNS for browser-based config
   * Connect to SSID "DA2-Gateway-Config" (pass: datn1234) then open
   * any URL or go directly to http://192.168.4.1/               */
  wifi_ap_start();
  web_config_handler_start(WEB_MODE_AP);
  captive_dns_start();
  ESP_LOGI(TAG, "Web portal: connect to \"DA2-Gateway-Config\" "
                "then open http://192.168.4.1/");

  
  current_mode = APP_MODE_CONFIG;
  ESP_LOGI(TAG, "CONFIG mode active");
}

/**
 * @brief Return to NORMAL mode by resetting the MCU.
 *
 * All tasks and peripherals are cleanly restarted through the normal
 * boot sequence. New config saved to NVS during CONFIG mode takes
 * effect automatically after reset.
 */
static void switch_to_normal_mode(void) {
  if (current_mode == APP_MODE_NORMAL) {
    ESP_LOGW(TAG, "Already in NORMAL mode");
    return;
  }

  ESP_LOGI(TAG, "==> Exiting CONFIG mode — resetting MCU");
  vTaskDelay(pdMS_TO_TICKS(200)); /* flush UART log */
  esp_restart();
}

/* =====================================================================
 *  app_main
 * ===================================================================== */

void app_main(void) {
  ESP_LOGI(TAG, "DA2_esp WAN Gateway v2.0.0 starting");
  /* --- NVS ------------------------------------------------------------ */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(ret);

  /* --- Core peripherals ----------------------------------------------- */
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
  uart_switch_init(); /* UART_SEL GPIO46: default to LAN MCU   */
  ESP_ERROR_CHECK(config_init());
  ESP_ERROR_CHECK(i2c_dev_support_init());

  /* Configure both IOX reset pins as outputs, and assert reset (active-low) */
  gpio_config_t iox_rst_cfg = {
      .pin_bit_mask = (1ULL << TCA6416A_RESET_PIN) | (1ULL << ADAPTER_RESET_GPIO),
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  gpio_config(&iox_rst_cfg);
  /* Pulse reset (active-high on this PCB): assert HIGH, then release LOW */
  gpio_set_level(TCA6416A_RESET_PIN, 1);
  gpio_set_level(ADAPTER_RESET_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  /* Deassert reset — bring LOW so devices enter normal operation */
  gpio_set_level(TCA6416A_RESET_PIN, 0);
  gpio_set_level(ADAPTER_RESET_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(50)); /* Allow TCA6416A to complete power-on sequence */
  tca_init();               /* Init on-board TCA6416A @0x20 */
  i2c_dev_support_scan();   /* Both IOXes should now appear  */

  stack_handler_init();     /* Init on-board (0x20) + probe adapter (0x21) */

  /* Turn on DC Fan via IOX P15 */
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_15, true);
  stack_handler_gpio_write(0, STACK_GPIO_PIN_15, true);

  /* Set direction for 3V3 and 5V power rails EN and turn them ON */
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_10, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_11, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_12, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_13, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_14, true);
  stack_handler_gpio_set_direction(1, STACK_GPIO_PIN_04, true);
  stack_handler_gpio_write(0, STACK_GPIO_PIN_10, true); /* P10 = EN_3V3 */
  stack_handler_gpio_write(0, STACK_GPIO_PIN_11, true); /* P11 = EN_5V */
  stack_handler_gpio_write(0, STACK_GPIO_PIN_12, false); /* P12 = RLED off */
  stack_handler_gpio_write(0, STACK_GPIO_PIN_13, false); /* P13 = GLED off */
  stack_handler_gpio_write(0, STACK_GPIO_PIN_14, false); /* P14 = BLED off */
  stack_handler_gpio_write(1, STACK_GPIO_PIN_04, true); /* ADAPTER POWER ON */
  config_init_wan_stack_id(); /* invalidate stale LTE config       */

  pwr_source_init();
  pwr_monitor_task_start(); /* Battery monitor + HMI updates (5s interval) */
  hmi_task_init();       /* HMI display: init state only  */
  hmi_task_enter_mode(); /* Route UART to HMI, init display */
  pcf8563_init();
  pcf8563_clear_voltage_low_flag();

  /* Assign handle BEFORE enabling interrupts (ISR must not see NULL)  */
  main_task_handle = xTaskGetCurrentTaskHandle();
  setup_gpio0_interrupt();
  setup_gpio3_interrupt();

  /* --- ESP-IDF network stack ------------------------------------------ */
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* --- Always-on tasks ------------------------------------------------ */
  uart_handler_register_mode_callback(uart_mode_switch_callback);
  config_handler_task_start();
  mcu_lan_handler_start();
  uart_handler_task_start();

  /* --- Operational tasks (NORMAL boot) -------------------------------- */
  config_internet_type_t current_internet_type = g_internet_type;

  internet_connect_start(current_internet_type);
  vTaskDelay(pdMS_TO_TICKS(10000)); /* wait for internet link        */
  server_connect_start(g_server_type);

  /* Start web config portal — browser-accessible at http://gateway.local/
   * (STA: user's browser on same LAN; LTE/Ethernet: binds to that IP)   */
  web_config_handler_start(WEB_MODE_STA);

  if (g_internet_type != CONFIG_INTERNET_LTE) {
    jtag_task_start();
  }

  ESP_LOGI(TAG, "System ready — NORMAL mode");
  ESP_LOGI(TAG, "  GPIO0 = toggle CONFIG/NORMAL");
  // ESP_LOGI(TAG, "  GPIO3  = toggle POWER/RGB LED");

  /* --- Main event loop ------------------------------------------------ */
  for (;;) {
    uint32_t notif = 0;
    xTaskNotifyWait(0, 0xFFFFFFFF, &notif, portMAX_DELAY);

    /* GPIO0 — toggle CONFIG / NORMAL */
    if (notif == NOTIFY_BUTTON_PRESS) {
      if (current_mode == APP_MODE_NORMAL) {
        switch_to_config_mode(&current_internet_type);
      } else {
        switch_to_normal_mode();
      }
    }

    /* GPIO3 — toggle battery source enable/disable */
    if (notif == NOTIFY_GPIO3_PRESS) {
      battery_source_enabled = !battery_source_enabled;
      pwr_source_set_battery_enable(battery_source_enabled);
      ESP_LOGI(TAG, "Battery source -> %s", battery_source_enabled ? "ENABLED" : "DISABLED");
      stack_handler_gpio_write(0, STACK_GPIO_PIN_10, battery_source_enabled); /* P10 = EN_3V3 */
      stack_handler_gpio_write(0, STACK_GPIO_PIN_11, battery_source_enabled); /* P11 = EN_5V */
      stack_handler_gpio_write(0, STACK_GPIO_PIN_12, !battery_source_enabled); /* P12 = RLED off */
      stack_handler_gpio_write(0, STACK_GPIO_PIN_13, !battery_source_enabled); /* P13 = GLED off */
      stack_handler_gpio_write(0, STACK_GPIO_PIN_14, !battery_source_enabled); /* P14 = BLED off */
      stack_handler_gpio_write(1, STACK_GPIO_PIN_04, battery_source_enabled); /* ADAPTER POWER*/
    }

    /* UART command — CONFIG or NORMAL */
    if (notif == NOTIFY_UART_MODE_SWITCH) {
      if (requested_mode == 0) {
        switch_to_config_mode(&current_internet_type);
      } else if (requested_mode == 1) {
        switch_to_normal_mode();
      }
      requested_mode = -1;
    }
  }
}
