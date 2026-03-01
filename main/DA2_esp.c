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
#define USB_SWITCH_PIN      GPIO_NUM_3

/* ===== Task Notification Values ======================================= */
/* Each must be a unique value — eSetValueWithOverwrite overwrites        */
#define NOTIFY_BUTTON_PRESS     1   /* GPIO45: toggle CONFIG/NORMAL       */
#define NOTIFY_POWER_MODE       2   /* GPIO38: toggle power rails         */
#define NOTIFY_UART_MODE_SWITCH 3   /* UART callback: CONFIG or NORMAL    */

/* ===== Application State ============================================== */
typedef enum {
    APP_MODE_NORMAL = 0,
    APP_MODE_CONFIG,
} app_mode_t;

static app_mode_t current_mode  = APP_MODE_NORMAL;
static int        requested_mode = -1;   /* set by uart_mode_switch_callback */
static uint32_t   last_isr_tick  = 0;

/**
 * @brief GPIO45 ISR - Button press
 */
static void gpio45_isr_handler(void *arg) {
    uint32_t now = xTaskGetTickCountFromISR();
    if ((now - last_isr_tick) >= pdMS_TO_TICKS(500)) {
        last_isr_tick = now;
        BaseType_t xTaskWoken = pdFALSE;
        if (main_task_handle) {
            xTaskNotifyFromISR(main_task_handle, NOTIFY_BUTTON_PRESS, eSetValueWithOverwrite, &xTaskWoken);
        }
        if (xTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

/**
 * @brief GPIO38 ISR - Button press
*/
static void gpio38_isr_handler(void *arg) {
    uint32_t now = xTaskGetTickCountFromISR();
    if ((now - last_isr_tick) >= pdMS_TO_TICKS(500)) {
        last_isr_tick = now;
        BaseType_t xTaskWoken = pdFALSE;
        if (main_task_handle) {
            xTaskNotifyFromISR(main_task_handle, NOTIFY_POWER_MODE, eSetValueWithOverwrite, &xTaskWoken);
        }
        if (xTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

void setup_gpio45_interrupt(void) {
    gpio_config_t gpio45_cfg = {
        .pin_bit_mask = BIT64(45),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    
    ESP_ERROR_CHECK(gpio_config(&gpio45_cfg));
    ESP_ERROR_CHECK(gpio_isr_handler_add(45, gpio45_isr_handler, NULL));
    
    ESP_LOGI(TAG, "GPIO45 button interrupt configured");
}

void setup_gpio38_interrupt(void) {
    gpio_config_t gpio38_cfg = {
        .pin_bit_mask = BIT64(38),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    
    ESP_ERROR_CHECK(gpio_config(&gpio38_cfg));
    ESP_ERROR_CHECK(gpio_isr_handler_add(38, gpio38_isr_handler, NULL));
    
    ESP_LOGI(TAG, "GPIO38 button interrupt configured");
}

/* =====================================================================
 *  USB Switch
 * ===================================================================== */

static void usb_switch_init(void)
{
    gpio_config_t usb_cfg = {
        .pin_bit_mask = (1ULL << USB_SWITCH_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&usb_cfg));
    gpio_set_level(USB_SWITCH_PIN, 0);  // Default LOW
    
    ESP_LOGI(TAG, "USB switch initialized (GPIO %d)", USB_SWITCH_PIN);
}

static void usb_switch_set(bool high)
{
    gpio_set_level(USB_SWITCH_PIN, high ? 1 : 0);
    ESP_LOGI(TAG, "USB switch -> %s", high ? "HOST" : "MODEM");
}

/* =====================================================================
 *  Server Task Helpers
 * ===================================================================== */

void server_connect_stop(config_server_type_t type)
{
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

void server_connect_start(config_server_type_t type)
{
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
        ESP_LOGW(TAG, "server_connect_start: unknown type %d, defaulting to MQTT", type);
        mqtt_handler_task_start();
        break;
    }
}

/* =====================================================================
 *  Internet Task Helpers
 * ===================================================================== */

static void internet_connect_start(config_internet_type_t type)
{
    switch (type) {
    case CONFIG_INTERNET_LTE:
        ESP_LOGI(TAG, "Starting LTE connection");
        usb_switch_init();
        usb_switch_set(false);              /* USB -> modem */
        vTaskDelay(pdMS_TO_TICKS(100));
        lte_connect_task_start();
        break;
    case CONFIG_INTERNET_WIFI:
        ESP_LOGI(TAG, "Starting WiFi connection");
        wifi_connect_task_start();
        break;
    case CONFIG_INTERNET_ETHERNET:
        /* Not yet implemented */
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
static void uart_mode_switch_callback(int mode)
{
    requested_mode = mode;
    if (main_task_handle) {
        xTaskNotify(main_task_handle, NOTIFY_UART_MODE_SWITCH, eSetValueWithOverwrite);
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
static void switch_to_config_mode(config_internet_type_t *internet_type)
{
    if (current_mode == APP_MODE_CONFIG) {
        ESP_LOGW(TAG, "Already in CONFIG mode");
        return;
    }

    ESP_LOGI(TAG, "==> Entering CONFIG mode");

    /* When LTE is active the modem owns the USB bus — stop it first */
    if (*internet_type == CONFIG_INTERNET_LTE) {
        ESP_LOGI(TAG, "Stopping LTE task before USB switch");
        lte_connect_task_stop();
        vTaskDelay(pdMS_TO_TICKS(10000));    /* wait for modem to release USB */
    }

    usb_switch_set(true);                   /* USB -> host / config tool */
    vTaskDelay(pdMS_TO_TICKS(100));
    jtag_task_start();

    led_show_yellow();
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
static void switch_to_normal_mode(void)
{
    if (current_mode == APP_MODE_NORMAL) {
        ESP_LOGW(TAG, "Already in NORMAL mode");
        return;
    }

    ESP_LOGI(TAG, "==> Exiting CONFIG mode — resetting MCU");
    vTaskDelay(pdMS_TO_TICKS(200));     /* flush UART log */
    esp_restart();
}

/* =====================================================================
 *  app_main
 * ===================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "DA2_esp WAN Gateway v1.0.1 starting");

    /* --- NVS ------------------------------------------------------------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(ret);

    /* --- Core peripherals ----------------------------------------------- */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(i2c_dev_support_init());

    tca_init();
    gpio_set_level(TCA6424A_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2c_dev_support_scan();

    stack_handler_init();           /* identify WAN connector            */
    config_init_wan_stack_id();     /* invalidate stale LTE config       */

    init_led_strip();
    pwr_source_init();
    led_on();
    pcf8563_init();
    pcf8563_clear_voltage_low_flag();

    /* Assign handle BEFORE enabling interrupts (ISR must not see NULL)  */
    main_task_handle = xTaskGetCurrentTaskHandle();
    setup_gpio45_interrupt();
    setup_gpio38_interrupt();

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
    bool                   power_rails_on         = false;

    oled_monitor_task_start();
    oled_monitor_update_internet_type(current_internet_type);

    internet_connect_start(current_internet_type);
    vTaskDelay(pdMS_TO_TICKS(10000));   /* wait for internet link        */
    server_connect_start(g_server_type);

    if (g_internet_type != CONFIG_INTERNET_LTE) {
        jtag_task_start();
    }

    ESP_LOGI(TAG, "System ready — NORMAL mode");
    ESP_LOGI(TAG, "  GPIO45 = toggle CONFIG/NORMAL  |  GPIO38 = toggle power rails");

    /* --- Main event loop ------------------------------------------------ */
    for (;;) {
        uint32_t notif = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &notif, portMAX_DELAY);

        /* GPIO45 — toggle CONFIG / NORMAL */
        if (notif == NOTIFY_BUTTON_PRESS) {
            if (current_mode == APP_MODE_NORMAL) {
                switch_to_config_mode(&current_internet_type);
            } else {
                switch_to_normal_mode();
            }
        }

        /* GPIO38 — toggle power rails */
        if (notif == NOTIFY_POWER_MODE) {
            power_rails_on = !power_rails_on;
            ESP_LOGI(TAG, "Power rails -> %s", power_rails_on ? "ON" : "OFF");
            pwr_source_set_1v8(power_rails_on);
            pwr_source_set_3v3(power_rails_on);
            pwr_source_set_5v0(power_rails_on);
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
