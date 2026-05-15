/*
 * DA2_esp — ESP32-S3 WAN Gateway Main Application
 */

#include "DA2_esp.h"
#include <esp_pm.h>
#include <esp_timer.h>

/* ===== Module Tag ===================================================== */
static const char *TAG = "MAIN";

/* ===== Task Handle ==================================================== */
TaskHandle_t main_task_handle = NULL;
/* ===== GPIO =========================================================== */
#define GPIO3_POWER_RGB_CTRL GPIO_NUM_3 /* Toggle power rails + RGB LED */
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
static TaskHandle_t rgb_status_task_handle = NULL;

/* Debounce timers + state */
static esp_timer_handle_t gpio0_debounce_timer = NULL;
static esp_timer_handle_t gpio3_debounce_timer = NULL;
static bool debounce_gpio0_active = false;
static bool debounce_gpio3_active = false;

#define GPIO0_DEBOUNCE_MS 50  /* GPIO0 debounce window (ms) */
#define GPIO3_DEBOUNCE_MS 50  /* GPIO3 debounce window (ms) */
#define GPIO_STABLE_SAMPLES 3 /* Number of stable readings before accept */
#define RGB_STATUS_UPDATE_MS 250
#define RGB_BLINK_FAST_MS 250
#define RGB_BLINK_SLOW_MS 500

static bool battery_source_enabled = true; /* GPIO3: battery FET state */
static bool s_ext_power_ok =
    true; /* VBUS present — updated by pwr_monitor callback */

static void power_rail_write(stack_gpio_pin_num_t pin, bool level);

/* =====================================================================
 *  Power Rail Wrapper
 *
 *  Five IOX pins must be forced OFF whenever the system runs on battery
 *  alone (no VBUS):
 *    STACK_GPIO_PIN_15  — DC Fan       (true = on)
 *    STACK_GPIO_PIN_11  — EN_5V        (true = on)
 *    STACK_GPIO_PIN_12  — RLED         (true = on, active-low circuit)
 *    STACK_GPIO_PIN_13  — GLED         (true = on)
 *    STACK_GPIO_PIN_14  — BLED         (true = on)
 *
 *  Use power_rail_write() in place of bare stack_handler_gpio_write()
 *  for any of these pins. When s_ext_power_ok is false, the wrapper
 *  still blocks Fan + EN_5V enables but keeps the RGB rail available so
 *  status warnings can be shown with short duty-cycle blinks.
 * ===================================================================== */

static void rgb_led_apply(bool red_on, bool green_on, bool blue_on) {
  power_rail_write(STACK_GPIO_PIN_12, !red_on);
  power_rail_write(STACK_GPIO_PIN_13, !green_on);
  power_rail_write(STACK_GPIO_PIN_14, !blue_on);
}

static bool rgb_blink_phase(uint32_t period_ms) {
  TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
  if (period_ticks == 0) {
    period_ticks = 1;
  }
  return ((xTaskGetTickCount() / period_ticks) & 0x1U) == 0;
}

static void rgb_status_refresh(void) {
  pwr_monitor_status_t pwr_status = {0};
  bool have_pwr_status = (pwr_monitor_get_status(&pwr_status) == ESP_OK);
  bool battery_critical = false;
  bool battery_low = false;
  bool wan_online = is_internet_connected;
  bool server_online =
      (mcu_lan_handler_get_internet_status() == INTERNET_STATUS_ONLINE);

  if (have_pwr_status && pwr_status.bat_present) {
    battery_critical = pwr_status.bat_critical_low;
    battery_low = battery_critical ||
            (pwr_status.bat_soc_pct > 0 &&
             pwr_status.bat_soc_pct <= PWR_BATT_LOW_SOC_PCT);
  }

  if (battery_critical) {
    bool led_on = rgb_blink_phase(RGB_BLINK_FAST_MS);
    rgb_led_apply(led_on, false, false);
    return;
  }

  if (!server_online) {
    bool led_on = rgb_blink_phase(wan_online ? RGB_BLINK_SLOW_MS
                                             : RGB_BLINK_FAST_MS);
    if (wan_online) {
      rgb_led_apply(false, false, led_on);
    } else {
      rgb_led_apply(led_on, false, led_on);
    }
    return;
  }

  if (battery_low) {
    bool led_on = rgb_blink_phase(RGB_BLINK_SLOW_MS);
    rgb_led_apply(led_on, led_on, false);
    return;
  }

  if (current_mode == APP_MODE_CONFIG) {
    bool led_on = rgb_blink_phase(RGB_BLINK_SLOW_MS);
    rgb_led_apply(false, led_on, led_on);
    return;
  }

  rgb_led_apply(false, true, false);
}

static void rgb_status_task(void *arg) {
  (void)arg;

  for (;;) {
    rgb_status_refresh();
    vTaskDelay(pdMS_TO_TICKS(RGB_STATUS_UPDATE_MS));
  }
}

static void rgb_status_task_start(void) {
  if (rgb_status_task_handle != NULL) {
    return;
  }

  BaseType_t rc = xTaskCreate(rgb_status_task, "rgb_status", 3072, NULL, 3,
                              &rgb_status_task_handle);
  if (rc != pdPASS) {
    rgb_status_task_handle = NULL;
    ESP_LOGE(TAG, "Failed to start RGB status task");
  }
}

/**
 * @brief Write an IOX power-rail pin, blocking enable if VBUS is absent.
 *        For all other pins the call passes through unconditionally.
 */
static void power_rail_write(stack_gpio_pin_num_t pin, bool level) {
  if (!s_ext_power_ok && level) {
    switch (pin) {
    case STACK_GPIO_PIN_15: /* fan    */
    case STACK_GPIO_PIN_11: /* EN_5V  */
      ESP_LOGD(TAG, "Rail P%d enable blocked (battery-only)", (int)pin);
      return;
    default:
      break;
    }
  }
  stack_handler_gpio_write(0, pin, level);
}

/**
 * @brief Enforce power-rail state on VBUS change.
 *
 * Called by the pwr_monitor callback when power_good transitions.
 *   ext_power_ok = false → battery only: immediately force fan + EN_5V OFF.
 *   ext_power_ok = true  → VBUS returned: restore fan (always on) and EN_5V
 *                          to their requested state.
 */
static void power_rails_apply(bool ext_power_ok) {
  if (s_ext_power_ok == ext_power_ok) {
    return; /* no change */
  }
  s_ext_power_ok = ext_power_ok;

  if (!ext_power_ok) {
    /* Battery only — force non-essential rails off immediately */
    stack_handler_gpio_write(0, STACK_GPIO_PIN_15, false); /* fan OFF  */
    stack_handler_gpio_write(0, STACK_GPIO_PIN_11, false); /* EN_5V OFF */
    ESP_LOGW(TAG, "Power rails reduced — battery-only mode");
  } else {
    /* External power returned — restore normal state */
    stack_handler_gpio_write(0, STACK_GPIO_PIN_15, true); /* fan always on */
    stack_handler_gpio_write(0, STACK_GPIO_PIN_11,
                             battery_source_enabled); /* EN_5V */
    ESP_LOGI(TAG, "Power rails restored — external power (batt_en=%d)",
             battery_source_enabled);
  }

  rgb_status_refresh();
}

/**
 * @brief GPIO0 debounce timer callback - confirm button press after stable
 * reads
 */
static void gpio0_debounce_callback(void *arg) {
  int stable_count = 0;
  int pin_state = -1;

  /* Sample GPIO0 multiple times to ensure stable state */
  for (int i = 0; i < GPIO_STABLE_SAMPLES; i++) {
    int current_read = gpio_get_level(0);
    if (pin_state == -1) {
      pin_state = current_read;
    }
    if (current_read == pin_state) {
      stable_count++;
    } else {
      break; /* pin is bouncing, abort debounce */
    }
    if (i < GPIO_STABLE_SAMPLES - 1) {
      vTaskDelay(pdMS_TO_TICKS(2)); /* small delay between samples */
    }
  }

  /* If pin remained stable and is LOW (pressed), post notification */
  if (stable_count >= GPIO_STABLE_SAMPLES && pin_state == 0) {
    BaseType_t xTaskWoken = pdFALSE;
    if (main_task_handle) {
      xTaskNotifyFromISR(main_task_handle, NOTIFY_BUTTON_PRESS,
                         eSetValueWithOverwrite, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }

  /* Re-enable GPIO0 interrupt for next press */
  debounce_gpio0_active = false;
  gpio_intr_enable(0);
}

/**
 * @brief GPIO3 debounce timer callback - confirm power button press after
 * stable reads
 */
static void gpio3_debounce_callback(void *arg) {
  int stable_count = 0;
  int pin_state = -1;

  /* Sample GPIO3 multiple times to ensure stable state */
  for (int i = 0; i < GPIO_STABLE_SAMPLES; i++) {
    int current_read = gpio_get_level(GPIO3_POWER_RGB_CTRL);
    if (pin_state == -1) {
      pin_state = current_read;
    }
    if (current_read == pin_state) {
      stable_count++;
    } else {
      break; /* pin is bouncing, abort debounce */
    }
    if (i < GPIO_STABLE_SAMPLES - 1) {
      vTaskDelay(pdMS_TO_TICKS(2)); /* small delay between samples */
    }
  }

  /* If pin remained stable and is LOW (pressed), post notification */
  if (stable_count >= GPIO_STABLE_SAMPLES && pin_state == 0) {
    BaseType_t xTaskWoken = pdFALSE;
    if (main_task_handle) {
      xTaskNotifyFromISR(main_task_handle, NOTIFY_GPIO3_PRESS,
                         eSetValueWithOverwrite, &xTaskWoken);
    }
    if (xTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }

  /* Re-enable GPIO3 interrupt for next press */
  debounce_gpio3_active = false;
  gpio_intr_enable(GPIO3_POWER_RGB_CTRL);
}

/**
 * @brief GPIO0 ISR - Start debounce timer
 */
static void gpio0_isr_handler(void *arg) {
  if (debounce_gpio0_active)
    return; /* debounce already active */

  debounce_gpio0_active = true;
  gpio_intr_disable(0); /* disable interrupt during debounce */

  /* Start debounce timer */
  esp_timer_start_once(gpio0_debounce_timer, GPIO0_DEBOUNCE_MS * 1000);
}

/**
 * @brief GPIO3 ISR - Start debounce timer
 */
static void gpio3_isr_handler(void *arg) {
  if (debounce_gpio3_active)
    return; /* debounce already active */

  debounce_gpio3_active = true;
  gpio_intr_disable(
      GPIO3_POWER_RGB_CTRL); /* disable interrupt during debounce */

  /* Start debounce timer */
  esp_timer_start_once(gpio3_debounce_timer, GPIO3_DEBOUNCE_MS * 1000);
}

void setup_gpio0_interrupt(void) {
  gpio_config_t gpio0_cfg = {.pin_bit_mask = BIT64(0),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .intr_type = GPIO_INTR_NEGEDGE};

  ESP_ERROR_CHECK(gpio_config(&gpio0_cfg));
  ESP_ERROR_CHECK(gpio_isr_handler_add(0, gpio0_isr_handler, NULL));

  /* Create debounce timer for GPIO0 (one-shot, not started yet) */
  esp_timer_create_args_t timer_args = {.callback = gpio0_debounce_callback,
                                        .name = "gpio0_debounce"};
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &gpio0_debounce_timer));

  ESP_LOGI(TAG, "GPIO0 button interrupt configured (debounce %d ms)",
           GPIO0_DEBOUNCE_MS);
}

void setup_gpio3_interrupt(void) {
  gpio_config_t gpio3_cfg = {.pin_bit_mask = BIT64(GPIO3_POWER_RGB_CTRL),
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .intr_type = GPIO_INTR_NEGEDGE};

  ESP_ERROR_CHECK(gpio_config(&gpio3_cfg));
  ESP_ERROR_CHECK(
      gpio_isr_handler_add(GPIO3_POWER_RGB_CTRL, gpio3_isr_handler, NULL));

  /* Create debounce timer for GPIO3 (one-shot, not started yet) */
  esp_timer_create_args_t timer_args = {.callback = gpio3_debounce_callback,
                                        .name = "gpio3_debounce"};
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &gpio3_debounce_timer));

  ESP_LOGI(TAG, "GPIO3 power/RGB interrupt configured (debounce %d ms)",
           GPIO3_DEBOUNCE_MS);
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

  /* Disable automatic light sleep FIRST. With light sleep enabled, stopping
   * tasks that have PSRAM-allocated stacks causes a race: the PM system
   * enters sleep (gating PSRAM) while the FreeRTOS idle task is still
   * freeing the deleted task's PSRAM stack, triggering a spinlock assert
   * inside spinlock_acquire (lock->count == 0 fires when count != 0).   */
#if CONFIG_PM_ENABLE
  {
    esp_pm_config_t pm_no_sleep = {
        .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    esp_pm_configure(&pm_no_sleep);
    ESP_LOGI(TAG, "CONFIG mode: light sleep disabled");
  }
#endif

  /* IMPORTANT: Stop all running server handlers and the active internet
   * connection BEFORE starting WiFi AP. The WiFi driver needs ~80 KB of
   * internal RAM for its init. After LTE PPP + MQTT are running this RAM
   * is exhausted, causing esp_wifi_init() to return ESP_ERR_NO_MEM and
   * the ESP_ERROR_CHECK to abort/crash.                                  */
  server_connect_stop(g_server_type);
  ESP_LOGI(TAG, "CONFIG mode: server stopped");

  /* Stop the STA web config server — it must be torn down before we can
   * start the AP portal (web_config_handler_start guards against double
   * start and would silently return ESP_ERR_INVALID_STATE otherwise).   */
  web_config_handler_stop();
  ESP_LOGI(TAG, "CONFIG mode: web server stopped");

  // switch (*internet_type) {
  // case CONFIG_INTERNET_LTE:
  //   lte_connect_task_stop();
  //   ESP_LOGI(TAG, "CONFIG mode: LTE stopped");
  //   break;
  // case CONFIG_INTERNET_ETHERNET:
  //   eth_connect_task_stop();
  //   ESP_LOGI(TAG, "CONFIG mode: Ethernet stopped");
  //   break;
  // default:
  //   break;
  // }

  /* Give TCP/IP stack time to release PPP buffers and close sockets */
  vTaskDelay(pdMS_TO_TICKS(500));

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
      .pin_bit_mask =
          (1ULL << TCA6416A_RESET_PIN) | (1ULL << ADAPTER_RESET_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&iox_rst_cfg);
  /* Pulse reset (active-high on this PCB): assert HIGH, then release LOW */
  gpio_set_level(TCA6416A_RESET_PIN, 1);
  gpio_set_level(ADAPTER_RESET_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  /* Deassert reset — bring LOW so devices enter normal operation */
  gpio_set_level(TCA6416A_RESET_PIN, 0);
  gpio_set_level(ADAPTER_RESET_GPIO, 0);
  vTaskDelay(
      pdMS_TO_TICKS(50)); /* Allow TCA6416A to complete power-on sequence */
  tca_init();             /* Init on-board TCA6416A @0x20 */
  i2c_dev_support_scan(); /* Both IOXes should now appear  */

  stack_handler_init(); /* Init on-board (0x20) + probe adapter (0x21) */

  /* Turn on DC Fan via IOX P15 */
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_15, true);
  power_rail_write(STACK_GPIO_PIN_15, true);

  /* Set direction for 3V3 and 5V power rails EN and turn them ON */
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_10, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_11, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_12, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_13, true);
  stack_handler_gpio_set_direction(0, STACK_GPIO_PIN_14, true);
  stack_handler_gpio_set_direction(1, STACK_GPIO_PIN_04, true);
  stack_handler_gpio_write(0, STACK_GPIO_PIN_10, true); /* P10 = EN_3V3 */
  power_rail_write(STACK_GPIO_PIN_11, true);            /* P11 = EN_5V  */
  power_rail_write(STACK_GPIO_PIN_12, false);            /* P12 = RLED on */
  power_rail_write(STACK_GPIO_PIN_13, false);            /* P13 = GLED on */
  power_rail_write(STACK_GPIO_PIN_14, false);            /* P14 = BLED on */
  stack_handler_gpio_write(1, STACK_GPIO_PIN_04, true); /* ADAPTER POWER ON */
  config_init_wan_stack_id(); /* invalidate stale LTE config       */

  pwr_source_init();
  pwr_monitor_task_start(); /* Battery monitor + HMI updates (5s interval) */

  /* Check VBUS state now and enforce rail policy immediately if on battery */
  {
    pwr_source_status_t boot_pwr = {0};
    if (pwr_source_get_status(&boot_pwr) == ESP_OK && !boot_pwr.power_good) {
      s_ext_power_ok = true; /* force a transition inside power_rails_apply */
      power_rails_apply(false);
    }
  }
  pwr_monitor_register_power_good_cb(power_rails_apply);
  rgb_status_task_start();
  hmi_task_init();          /* HMI display: init state only  */
  {
    esp_err_t hmi_ret = hmi_task_enter_mode(); /* Route UART to HMI, init display */
    if (hmi_ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enter HMI mode: %s", esp_err_to_name(hmi_ret));
    }
  }
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

  /* Wait until the internet link is UP and time is synced before starting */
  {
    int timeout_s = 10;
    if (current_internet_type == CONFIG_INTERNET_LTE) timeout_s = 30;
    ESP_LOGI(TAG, "Waiting for internet connection and time sync (max %d s)...",
             timeout_s);
    for (int waited = 0; waited < timeout_s; waited++) {
      bool synced = false;
      switch (current_internet_type) {
      case CONFIG_INTERNET_LTE:
        synced = lte_is_sntp_synced();
        break;
      case CONFIG_INTERNET_WIFI:
        synced = wifi_is_sntp_synced();
        break;
      case CONFIG_INTERNET_ETHERNET:
        synced = eth_is_sntp_synced();
        break;
      default:
        synced = true;
        break;
      }
      if (is_internet_connected && synced) {
        ESP_LOGI(
            TAG,
            "Internet connected and time synced after %d s — starting servers",
            waited);
        break;
      }
      if (waited == timeout_s - 1) {
        ESP_LOGW(TAG, "Timed out waiting for connection/time sync — starting "
                      "servers anyway");
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  internet_monitor_task_start(); /* start fallback monitor if enabled */

  /* Start web config portal BEFORE protocol handlers — httpd needs to
   * allocate its task stack from internal RAM, which is more constrained
   * once a connected LTE PPP stack + MQTT tasks are running.            */
  web_config_handler_start(WEB_MODE_STA);

  server_connect_start(g_server_type);

  ESP_LOGI(TAG, "System ready — NORMAL mode");
  ESP_LOGI(TAG, "  GPIO0 = toggle CONFIG/NORMAL");
  ESP_LOGI(TAG, "  GPIO3 = toggle BATTERY SOURCE enable/disable");

// Enable sleep mode:
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 240, .min_freq_mhz = 40, .light_sleep_enable = true};
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
  ESP_LOGI(TAG, "Automatic Light Sleep & Power Management ENABLED");
#endif

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
      ESP_LOGI(TAG, "Battery source -> %s",
               battery_source_enabled ? "ENABLED" : "DISABLED");
      stack_handler_gpio_write(0, STACK_GPIO_PIN_10,
                               battery_source_enabled); /* P10 = EN_3V3 */
      power_rail_write(STACK_GPIO_PIN_11,
                       battery_source_enabled); /* P11 = EN_5V  */
      stack_handler_gpio_write(1, STACK_GPIO_PIN_04,
                               battery_source_enabled); /* ADAPTER POWER*/
      rgb_status_refresh();
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

// WiFi lab : TickLab_2.4G2 PASS: T1ckL@b5917