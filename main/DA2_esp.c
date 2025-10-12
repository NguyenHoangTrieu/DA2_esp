/*
 * DA2_esp.c - Main entry point for ESP32-P4 modular application.
 * This file sets up NVS, WiFi scan, WiFi UART connect, and MQTT send.
 * The scan task runs until WiFi connects via UART command, after which it is suspended.
 * If connection fails or is lost, scan resumes. MQTT sending only happens when WiFi is up.
 * All logic is modular; all comments are in English for clarity.
 */

#include "wifi_scan.h"
#include "wifi_connect.h"
#include "mqtt_handler.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "DA2_esp_main";

void app_main(void)
{
    // Initialize NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize system event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Starting WiFi scan task (scans every 5s until connected)...");
    wifi_scan_start();

    ESP_LOGI(TAG, "Starting WiFi UART connect task (waiting for connect command)...");
    wifi_connect_task_start();

    ESP_LOGI(TAG, "Starting MQTT handler (suspended by default, resumes on WiFi connect)...");
    mqtt_handle_start();
}
