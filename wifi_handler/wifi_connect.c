/*
 * WiFi connection module for ESP32-P4 board.
 * Implements WiFi initialization (station mode), UART credential parsing,
 * and hardware reset control for ESP32-C6 slave chip.
 * All comments are in English for maintainability.
 */

#include "wifi_connect.h"
#include "wifi_scan.h"
#include "mqtt_handler.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"

#define DEFAULT_ESP_WIFI_SSID      "Devil"     // Initial hardcoded SSID
#define DEFAULT_ESP_WIFI_PASS      "hamhap7604"// Initial hardcoded password
#define EXAMPLE_ESP_MAXIMUM_RETRY  10

// Adjust for the security/auth your AP uses
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER ""
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
// Uncomment if WPA3 support needed
// #define ESP_WIFI_WPA3_COMPATIBLE_SUPPORT

static EventGroupHandle_t s_wifi_event_group; // FreeRTOS event group to signal WiFi state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define C6_RESET_GPIO      GPIO_NUM_54      // GPIO for ESP32-C6 slave reset

#define UART_PORT_NUM      UART_NUM_0
#define UART_BAUD_RATE     115200
#define UART_BUF_SIZE      256

static const char *TAG = "wifi station";
static int s_retry_num = 0;

static char s_wifi_ssid[33] = DEFAULT_ESP_WIFI_SSID; // Buffer for current SSID
static char s_wifi_pass[65] = DEFAULT_ESP_WIFI_PASS; // Buffer for current password
static uint8_t s_wifi_connected = 0;               // Connection status flag

/*
 * WiFi event handler monitors connection events, initiates reconnect,
 * and signals status in FreeRTOS event group.
 */
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
        ESP_LOGI(TAG, "Disconnected from WiFi MQTT suspended, WiFi scan resumed");
        mqtt_handle_suspend(); // Suspend MQTT task on disconnect
        s_wifi_connected = 0;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        mqtt_handle_resume(); // Resume MQTT task on successful connection
        s_wifi_connected = 1;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/*
 * Initialize WiFi in STA mode and connect.
 * Custom SSID/PASSWORD can be provided for initial connection.
 */
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

    // Wait for either successful connection or maximum retry/failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

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

/*
 * FreeRTOS UART task:
 * Listens for WiFi credentials in format SSID:PASSWORD.
 * Updates connection if a valid command is received.
 */
static void wifi_uart_task(void *arg)
{
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
    uint16_t scan_counter = 0;

    uint8_t data[UART_BUF_SIZE];
    while (1) {
        if (!s_wifi_connected && scan_counter == 200) {
            perform_scan(); // Scan for available networks if not connected
            scan_counter = 0;
        }
        else if (!s_wifi_connected) {
            scan_counter++;
        }
        else {
           scan_counter = 0;
        }
        int len = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
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

                    // Connect to new WiFi network with parsed credentials
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
                    ESP_ERROR_CHECK(esp_wifi_disconnect()); // Ensures disconnect before reconnect
                    ESP_ERROR_CHECK(esp_wifi_connect());
                }
            } else {
                ESP_LOGI(TAG, "Invalid format. Usage: SSID:PASSWORD");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void wifi_connect_task_start(void)
{
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (initial connection)");
    wifi_init_sta(s_wifi_ssid, s_wifi_pass);
    // Start the UART task to listen for WiFi credentials
    xTaskCreate(wifi_uart_task, "wifi_uart_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "WiFi UART task created");
}