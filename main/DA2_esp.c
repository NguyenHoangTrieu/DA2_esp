/*
 * ESP32-S3 Gateway Main Application
 */

#include "DA2_esp.h"

static const char *TAG = "MAIN APP";

TaskHandle_t main_task_handle = NULL;

/* Notification values */
#define NOTIFY_BUTTON_PRESS   1
#define NOTIFY_UART_MODE_SWITCH 2

/* App modes */
typedef enum {
    APP_MODE_NORMAL = 0,
    APP_MODE_CONFIG = 1,
} app_mode_t;

static app_mode_t current_mode = APP_MODE_NORMAL;
static int requested_mode = -1;  // For UART mode switch

static uint32_t last_isr_tick = 0;

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
 * @brief UART mode switch callback
 * Called from uart_handler when CONFIG/NORMAL detected
 * @param mode 0=CONFIG, 1=NORMAL
 */
static void uart_mode_switch_callback(int mode) {
    requested_mode = mode;
    
    // Notify main task
    if (main_task_handle) {
        xTaskNotify(main_task_handle, NOTIFY_UART_MODE_SWITCH, eSetValueWithOverwrite);
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
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(45, gpio45_isr_handler, NULL));
    
    ESP_LOGI(TAG, "GPIO45 button interrupt configured");
}

/**
 * @brief Switch to CONFIG mode
 */
static void switch_to_config_mode(config_internet_type_t *current_internet_type) {
    if (current_mode == APP_MODE_CONFIG) {
        ESP_LOGW(TAG, "Already in CONFIG mode");
        return;
    }
    
    ESP_LOGI(TAG, "==> Switching to CONFIG mode");
    
    // usb_otg_rw_task_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    class_driver_task_stop();
    usb_host_lib_task_stop();
    mqtt_handler_task_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    jtag_task_start();
    config_handler_task_start();
    led_show_yellow();
    
    current_mode = APP_MODE_CONFIG;
    ESP_LOGI(TAG, "CONFIG mode active");
}

/**
 * @brief Switch to NORMAL mode
 */
static void switch_to_normal_mode(config_internet_type_t *current_internet_type) {
    if (current_mode == APP_MODE_NORMAL) {
        ESP_LOGW(TAG, "Already in NORMAL mode");
        return;
    }
    
    ESP_LOGI(TAG, "==> Switching to NORMAL mode");
    
    jtag_task_stop();
    config_handler_task_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    usb_host_lib_task_start();
    class_driver_task_start();
    // usb_otg_rw_task_start();
    
    if (*current_internet_type != g_internet_type) {
        ESP_LOGI(TAG, "Internet type changed: %d -> %d", *current_internet_type, g_internet_type);
        *current_internet_type = g_internet_type;
        
        switch (*current_internet_type) {
        case CONFIG_INTERNET_LTE:
            wifi_connect_task_stop();
            lte_connect_task_start();
            break;
        case CONFIG_INTERNET_WIFI:
            lte_connect_task_stop();
            wifi_connect_task_start();
            break;
        case CONFIG_INTERNET_ETHERNET:
            lte_connect_task_stop();
            wifi_connect_task_stop();
            // Ethernet task start can be added here
            ESP_LOGI(TAG, "Ethernet selected - no task implemented");
            break;
        default:
            ESP_LOGW(TAG, "Unknown internet type: %d", *current_internet_type);
            break;
        }
    }
    
    mqtt_handler_task_start();
    led_show_orange();
    
    current_mode = APP_MODE_NORMAL;
    ESP_LOGI(TAG, "NORMAL mode active");
}

void app_main(void) {
    ESP_LOGI(TAG, "Firmware Version: DA2_esp v1.0.1");
    
    init_led_strip();
    led_on();
    
    setup_gpio45_interrupt();

    main_task_handle = xTaskGetCurrentTaskHandle();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register UART mode switch callback
    uart_handler_register_mode_callback(uart_mode_switch_callback);
    
    config_internet_type_t current_internet_type = g_internet_type;
    
    // NVS Initialize
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Start USB tasks
    usb_host_lib_task_start();
    class_driver_task_start();
    // usb_otg_rw_task_start();

    // Start Internet tasks
    switch (current_internet_type) {
    case CONFIG_INTERNET_LTE:
        lte_connect_task_start();
        break;
    case CONFIG_INTERNET_WIFI:
        wifi_connect_task_start();
        break;
    default:
        ESP_LOGW(TAG, "Unknown internet type, no internet task started");
        break;
    }
    
    uart_handler_task_start();
    mqtt_handler_task_start();

    ESP_LOGI(TAG, "System started in NORMAL mode");
    ESP_LOGI(TAG, "Press GPIO45 or send UART 'CONFIG'/'NORMAL' to switch modes");

    // Main event loop
    while (1) {
        uint32_t notification_value = 0;
        
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &notification_value, portMAX_DELAY)) {
            
            // Button press - toggle mode
            if (notification_value == NOTIFY_BUTTON_PRESS) {
                ESP_LOGI(TAG, "Button pressed - toggling mode");
                
                if (current_mode == APP_MODE_NORMAL) {
                    switch_to_config_mode(&current_internet_type);
                } else {
                    switch_to_normal_mode(&current_internet_type);
                }
            }
            
            // UART mode switch command
            else if (notification_value == NOTIFY_UART_MODE_SWITCH) {
                if (requested_mode == 0) {
                    // CONFIG mode requested
                    switch_to_config_mode(&current_internet_type);
                } 
                else if (requested_mode == 1) {
                    // NORMAL mode requested
                    switch_to_normal_mode(&current_internet_type);
                }
                
                requested_mode = -1;  // Reset
            }
        }
    }
}
