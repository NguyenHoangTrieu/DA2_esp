/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "driver/uart.h"

/* WiFi configuration - these are initial values. 
   Can be changed by UART command at runtime. */
#define DEFAULT_ESP_WIFI_SSID      "Devil"
#define DEFAULT_ESP_WIFI_PASS      "hamhap7604"
#define EXAMPLE_ESP_MAXIMUM_RETRY  10

// WPA3-SAE mode settings. Set as needed.
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER ""    // Used for WPA3 H2E, otherwise keep empty

// Authmode threshold. Adjust as needed.
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

// Uncomment if you use WPA3-compatible support
// #define ESP_WIFI_WPA3_COMPATIBLE_SUPPORT

// FreeRTOS event group to signal WiFi state
static EventGroupHandle_t s_wifi_event_group;

// The event group uses two bits for WiFi connection state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// GPIO for ESP32-C6 slave reset control on Waveshare board
#define C6_RESET_GPIO GPIO_NUM_54

// UART parameters for WiFi credential input
#define UART_PORT_NUM      UART_NUM_0
#define UART_BAUD_RATE     115200
#define UART_BUF_SIZE      256

static const char *TAG = "wifi station";
static int s_retry_num = 0;

/* Buffers to store SSID and password */
static char s_wifi_ssid[33] = DEFAULT_ESP_WIFI_SSID;
static char s_wifi_pass[65] = DEFAULT_ESP_WIFI_PASS;

/* WiFi event handler function */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize WiFi station and start connection */
void wifi_init_sta(const char *custom_ssid, const char *custom_pass)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, custom_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, custom_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
    strcpy((char *)wifi_config.sta.sae_h2e_identifier, EXAMPLE_H2E_IDENTIFIER);
#ifdef ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
    wifi_config.sta.disable_wpa3_compatible_mode = 0;
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Wait until either the connection is established (WIFI_CONNECTED_BIT)
       or connection failed for the maximum number of retries (WIFI_FAIL_BIT).
       These bits are set by event_handler() (see above). */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned,
       hence we can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s Password:%s",
                 custom_ssid, custom_pass);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, Password:%s",
                 custom_ssid, custom_pass);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

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
    // Perform reset sequence with multiple toggles for robust reset
    for (int i = 0; i < 3; i++) {
        gpio_set_level(C6_RESET_GPIO, 0);  // Assert reset (LOW)
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(C6_RESET_GPIO, 1);  // De-assert reset (HIGH)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Final reset: hold low for 500ms then release
    gpio_set_level(C6_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(C6_RESET_GPIO, 1);
    // Wait for C6 to boot up completely (critical timing)
    ESP_LOGI(TAG, "Waiting for ESP32-C6 to boot up...");
    vTaskDelay(pdMS_TO_TICKS(2000));  // 2 seconds for C6 bootloader + app init
    ESP_LOGI(TAG, "ESP32-C6 slave reset completed");
}

/* Task to receive SSID/Password via UART and connect to WiFi.
   Command format: SSID:PASSWORD (just send this string, no '\n' is needed) */
static void wifi_uart_task(void *arg)
{
    // UART configuration
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);

    ESP_LOGI(TAG, "Listening for WiFi SSID/Password over UART (format: SSID:PASSWORD)");

    uint8_t data[UART_BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            // Parse input by finding first ':' and splitting SSID/PASSWORD
            char *colon_ptr = strchr((char *)data, ':');
            if (colon_ptr) {
                int ssid_len = colon_ptr - (char *)data;
                int pass_len = strlen((char *)data) - ssid_len - 1;
                if (ssid_len > 0 && ssid_len < 33 && pass_len >= 0 && pass_len < 65) {
                    strncpy(s_wifi_ssid, (char *)data, ssid_len);
                    s_wifi_ssid[ssid_len] = '\0';
                    strncpy(s_wifi_pass, colon_ptr + 1, pass_len);
                    s_wifi_pass[pass_len] = '\0';
                    ESP_LOGI(TAG, "Received command: SSID='%s', PASS='%s'", s_wifi_ssid, s_wifi_pass);

                    /* Connect to new WiFi network */
                    wifi_config_t wifi_config = {0};
                    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid));
                    strncpy((char *)wifi_config.sta.password, s_wifi_pass, sizeof(wifi_config.sta.password));
                    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
                    wifi_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
                    strcpy((char *)wifi_config.sta.sae_h2e_identifier, EXAMPLE_H2E_IDENTIFIER);

    #ifdef ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
                    wifi_config.sta.disable_wpa3_compatible_mode = 0;
    #endif
                    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                    ESP_ERROR_CHECK(esp_wifi_disconnect()); // Ensure disconnect before reconnect
                    ESP_ERROR_CHECK(esp_wifi_connect());
                }
            } else {
                ESP_LOGI(TAG, "Invalid format. Usage: SSID:PASSWORD");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Main entry point */
void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // CRITICAL: Reset ESP32-C6 slave before WiFi initialization
    reset_c6_slave();

    // Initialize WiFi with default SSID/PASS at boot
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (initial connection)");
    wifi_init_sta(s_wifi_ssid, s_wifi_pass);

    // Start UART task to listen for credentials and connect dynamically
    xTaskCreate(wifi_uart_task, "wifi_uart_task", 4096, NULL, 5, NULL);
}
