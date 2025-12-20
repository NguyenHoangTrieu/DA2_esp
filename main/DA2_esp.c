/*
 * ESP32-S3 Gateway Main Application
 */

#include "DA2_esp.h"

static const char *TAG = "MAIN APP";

TaskHandle_t main_task_handle = NULL;

/* Notification values */
#define NOTIFY_BUTTON_PRESS   1
#define NOTIFY_POWER_MODE     2
#define NOTIFY_UART_MODE_SWITCH 2
#define USB_SWITCH_PIN    GPIO_NUM_3

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

static void usb_switch_init(void) {
    gpio_config_t usb_cfg = {
        .pin_bit_mask = (1ULL << USB_SWITCH_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    ESP_ERROR_CHECK(gpio_config(&usb_cfg));
    gpio_set_level(USB_SWITCH_PIN, 0);  // Default LOW
    
    ESP_LOGI(TAG, "USB switch initialized (GPIO %d)", USB_SWITCH_PIN);
}

static void usb_switch_set(bool level) {
    gpio_set_level(USB_SWITCH_PIN, level);
    ESP_LOGI(TAG, "USB switch set to %d", level);
}

/**
 * @brief Switch to CONFIG mode
 */
static void switch_to_config_mode(config_internet_type_t *current_internet_type) {
    if (current_mode == APP_MODE_CONFIG) {
        ESP_LOGW(TAG, "Already in CONFIG mode");
        return;
    }
    
    ESP_LOGI(TAG, "==> Switching to Straight CONFIG mode");
    // if (*current_internet_type != CONFIG_INTERNET_LTE) jtag_task_start();
    led_show_yellow();
    current_mode = APP_MODE_CONFIG;
    ESP_LOGI(TAG, "CONFIG mode active");
}

// Helper functions to start/stop internet and server connections
static void server_connect_start(config_server_type_t server_type){
    switch(server_type){
        case CONFIG_SERVERTYPE_MQTT:
            mqtt_handler_task_start();
            break;
        case CONFIG_SERVERTYPE_COAP:
            //coap_handler_task_start(); // To be implemented
            break;
        case CONFIG_SERVERTYPE_HTTP:
            //http_handler_task_start(); // To be implemented
            break; 
        default:
            ESP_LOGW(TAG, "Unknown server type: %d", server_type);
            break;
    }
}

// Helper functions to start/stop internet and server connections
void server_connect_stop(config_server_type_t server_type){
    switch(server_type){
        case CONFIG_SERVERTYPE_MQTT:
            mqtt_handler_task_stop();
            break;
        case CONFIG_SERVERTYPE_COAP:
            //coap_handler_task_stop(); // To be implemented
            break;
        case CONFIG_SERVERTYPE_HTTP:
            //http_handler_task_stop(); // To be implemented
            break; 
        default:
            ESP_LOGW(TAG, "Unknown server type: %d", g_server_type);
            break;
    }
}

// // Helper functions to start/stop internet connections
// static void internet_connect_stop(config_internet_type_t internet_type){
//     mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
//     switch(internet_type){
//         case CONFIG_INTERNET_LTE:
//             lte_connect_task_stop();
//             break;
//         case CONFIG_INTERNET_WIFI:
//             wifi_connect_task_stop();
//             break;
//         case CONFIG_INTERNET_ETHERNET:
//             //ethernet_connect_task_stop(); // To be implemented
//             break; 
//         default:
//             ESP_LOGW(TAG, "Unknown internet type: %d", internet_type);
//             break;
//     }
// }

// Helper functions to start/stop internet connections
static void internet_connect_start(config_internet_type_t internet_type){
    switch(internet_type){
        case CONFIG_INTERNET_LTE:
            ESP_LOGI(TAG, "Starting PPP server for LTE");
            usb_switch_init();
            usb_switch_set(false);  // Disable USB connection
            vTaskDelay(pdMS_TO_TICKS(100));
            lte_connect_task_start();
            break;
        case CONFIG_INTERNET_WIFI:
            ESP_LOGI(TAG, "Starting WiFi connection");
            wifi_connect_task_start();
            break;
        case CONFIG_INTERNET_ETHERNET:
            //ethernet_connect_task_start(); // To be implemented
            break; 
        default:
            ESP_LOGW(TAG, "Unknown internet type: %d", internet_type);
            break;
    }
}

/**
 * @brief Switch to NORMAL mode
 */
static void switch_to_normal_mode(config_internet_type_t *current_internet_type, config_server_type_t *current_server_type) {
    if (current_mode == APP_MODE_NORMAL) {
        ESP_LOGW(TAG, "Already in NORMAL mode");
        return;
    }
    
    ESP_LOGI(TAG, "==> Switching to NORMAL mode");
    
    // if (*current_internet_type != CONFIG_INTERNET_LTE) jtag_task_stop();

    vTaskDelay(pdMS_TO_TICKS(100));

    ppp_server_deinit();
    
    if (*current_internet_type != g_internet_type) {
        // ESP_LOGI(TAG, "Internet type changed: %d -> %d", *current_internet_type, g_internet_type);
        // config_internet_type_t old_type = *current_internet_type;
        // *current_internet_type = g_internet_type;
        // server_connect_stop(*current_server_type);
        // vTaskDelay(pdMS_TO_TICKS(5000));
        // for (int i = 0; i < CONFIG_INTERNET_COUNT; i++) {
        //     if (i == old_type) {
        //         internet_connect_stop(i);
        //     }
        // }
        // internet_connect_start(*current_internet_type);
        // ESP_LOGI(TAG, "Waiting for internet connection...");
        // while(is_internet_connected == false){
        //     vTaskDelay(pdMS_TO_TICKS(100));
        // }
        // server_connect_start(*current_server_type);
    }
    if (*current_server_type != g_server_type) {
        ESP_LOGI(TAG, "Server type changed: %d -> %d", *current_server_type, g_server_type);
        config_server_type_t old_type = *current_server_type;
        *current_server_type = g_server_type;
        for (int i = 0; i < CONFIG_SERVERTYPE_COUNT; i++) {
            if (i == old_type) {
                server_connect_stop(i);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        server_connect_start(*current_server_type);
    }
    led_show_blue();
    
    current_mode = APP_MODE_NORMAL;
    ESP_LOGI(TAG, "NORMAL mode active");
}

void app_main(void) {
    ESP_LOGI(TAG, "Firmware Version: DA2_esp v1.0.1");
    
    // NVS Initialize
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(i2c_dev_support_init());
    tca_init();
    gpio_set_level(TCA6424A_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    i2c_dev_support_scan(); // Scan I2C bus for devices
    init_led_strip();
    pwr_source_init();
    led_on();
    pcf8563_init();
    pcf8563_clear_voltage_low_flag();
    setup_gpio45_interrupt();
    setup_gpio38_interrupt();
    main_task_handle = xTaskGetCurrentTaskHandle();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register UART mode switch callback
    uart_handler_register_mode_callback(uart_mode_switch_callback);
    
    config_internet_type_t current_internet_type = g_internet_type;
    config_server_type_t current_server_type = g_server_type;

    config_handler_task_start();
    mcu_lan_handler_start();
    uart_handler_task_start();
    
    // Start OLED monitor
    oled_monitor_task_start();
    oled_monitor_update_internet_type(g_internet_type);
    // Start Internet tasks
    internet_connect_start(current_internet_type);
    vTaskDelay(pdMS_TO_TICKS(10000));
    mqtt_handler_task_start();
    volatile bool switch_mode = true;

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
                    switch_to_normal_mode(&current_internet_type, &current_server_type);
                }
            }

            if (notification_value == NOTIFY_POWER_MODE) {
                // Power mode change handling can be implemented here
                ESP_LOGI(TAG, "Power mode change notification received");
                if (switch_mode) {
                    ESP_LOGI(TAG, "Switching to POWER mode");
                    // Implement low power mode actions
                    pwr_source_set_1v8(true);
                    pwr_source_set_3v3(true);
                    pwr_source_set_5v0(true);
                    switch_mode = false;
                } else {
                    ESP_LOGI(TAG, "Switching to OFF POWER mode");
                    // Implement normal power mode actions
                    pwr_source_set_1v8(false);
                    pwr_source_set_3v3(false);
                    pwr_source_set_5v0(false);
                    switch_mode = true;
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
                    switch_to_normal_mode(&current_internet_type, &current_server_type);
                }
                
                requested_mode = -1;  // Reset
            }
        }
    }
}
