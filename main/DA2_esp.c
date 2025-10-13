/*
 * DA2_esp.c - Main entry point for ESP32-P4 modular application.
 * This file sets up NVS, WiFi scan, WiFi UART connect, and MQTT send.
 * The scan task runs until WiFi connects via UART command, after which it is suspended.
 * If connection fails or is lost, scan resumes. MQTT sending only happens when WiFi is up.
 * All logic is modular; all comments are in English for clarity.
 */

#include "wifi_scan.h"
#include "sensor_handler.h"
#include "wifi_connect.h"
#include "mqtt_handler.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define C6_RESET_GPIO   GPIO_NUM_54
static const char *TAG = "DA2_esp_main";

/* Software reset ESP32-C6 slave via GPIO54 */
static void reset_c6_slave(void)
{
    ESP_LOGI(TAG, "Performing software reset of ESP32-C6 slave...");
    
    // Configure GPIO54 as output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << C6_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Perform reset sequence - multiple toggles for robust reset
    for (int i = 0; i < 3; i++) {
        gpio_set_level(C6_RESET_GPIO, 0);  // Assert reset (LOW)
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(C6_RESET_GPIO, 1);  // De-assert reset (HIGH)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Final reset - hold low for 500ms then release
    gpio_set_level(C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(C6_RESET_GPIO, 1);
    
    // Wait for C6 to boot up completely (critical timing)
    ESP_LOGI(TAG, "Waiting for ESP32-C6 to boot up...");
    vTaskDelay(pdMS_TO_TICKS(2000));  // 2 seconds for C6 bootloader + app init
    
    ESP_LOGI(TAG, "ESP32-C6 slave reset completed");
}

void app_main(void)
{
    // Initialize NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // CRITICAL: Reset ESP32-C6 slave before WiFi initialization
    // This ensures C6 is in a known good state after P4 reset
    ESP_LOGI(TAG, "Starting ESP32-P4 with ESP32-C6 slave reset sequence");
    reset_c6_slave();

    // ESP_LOGI(TAG, "Starting WiFi UART connect task (waiting for connect command)...");
    // wifi_connect_task_start();

    // ESP_LOGI(TAG, "Starting MQTT handler (suspended by default, resumes on WiFi connect)...");
    // mqtt_handle_start();

    ESP_LOGI(TAG, "Starting sensor handler task (reads sensors, controls relay, builds telemetry)...");
    sensor_handler_start();
}
