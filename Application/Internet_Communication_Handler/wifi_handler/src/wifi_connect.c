/*
 * Wifi Connect Handler for esp32s3
 */

#include "wifi_connect.h"
#include "config_handler.h"

#define DEFAULT_ESP_WIFI_SSID "Devil"  // Initial hardcoded SSID
#define DEFAULT_ESP_WIFI_PASS "hamhap7604" // Initial hardcoded password
#define EXAMPLE_ESP_MAXIMUM_RETRY 3

// Adjust for the security/auth your AP uses
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER ""
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
// Uncomment if WPA3 support needed
// #define ESP_WIFI_WPA3_COMPATIBLE_SUPPORT

static EventGroupHandle_t
    s_wifi_event_group; // FreeRTOS event group to signal WiFi state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";
static int s_retry_num = 0;

static char s_wifi_ssid[33] = DEFAULT_ESP_WIFI_SSID; // Buffer for current SSID
static char s_wifi_pass[65] =
    DEFAULT_ESP_WIFI_PASS;           // Buffer for current password
static uint8_t s_wifi_connected = 0; // Connection status flag
static volatile uint8_t s_reconnect_request =
    0;                                       // Flag for reconnection request
static wifi_config_t s_pending_config = {0}; // Pending WiFi config
static bool wifi_connect_task_close = false;
static esp_netif_t* s_wifi_netif = NULL;

/*
 * WiFi event handler monitors connection events, initiates reconnect,
 * and signals status in FreeRTOS event group.
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    // Check if this is a requested reconnection with new credentials
    if (s_reconnect_request) {
      s_reconnect_request = 0; // Reset flag
      s_retry_num = 0;         // Reset retry counter for new connection

      // Apply the new configuration
      ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_pending_config));
      esp_wifi_connect();
      ESP_LOGI(TAG, "Connecting to new AP: %s", s_pending_config.sta.ssid);
    } else if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "Retrying to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    ESP_LOGI(TAG, "Connect to the AP failed");
    ESP_LOGI(TAG, "Disconnected from WiFi, Scan resumed");
    s_wifi_connected = 0;
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_wifi_connected = 1;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

/*
 * Initialize WiFi in STA mode and connect.
 * Custom SSID/PASSWORD can be provided for initial connection.
 */
void wifi_init_sta(const char *custom_ssid, const char *custom_pass) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  if (s_wifi_netif) {
        esp_netif_destroy(s_wifi_netif);
        s_wifi_netif = NULL;
    }
  s_wifi_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, custom_ssid,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, custom_pass,
          sizeof(wifi_config.sta.password));
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
                                         pdFALSE, pdFALSE, 1000);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to AP SSID:%s Password:%s", custom_ssid,
             custom_pass);
    s_wifi_connected = true;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, Password:%s", custom_ssid,
             custom_pass);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

/*
 * FreeRTOS WiFi config task:
 * Listens for WiFi credentials from config queue.
 * Updates connection if a valid config is received.
 */
static void wifi_config_task(void *arg) {
  ESP_LOGI(TAG, "WiFi config task started, listening for config from queue");
  uint16_t scan_counter = 0;
  wifi_config_data_t wifi_cfg;

  while (!wifi_connect_task_close) {
    // Perform scan if not connected
    if (!s_wifi_connected && scan_counter == 200) {
      ESP_LOGI(TAG, "Not connected, performing WiFi scan...");
      perform_scan(); // Scan for available networks if not connected
      scan_counter = 0;
    } else if (!s_wifi_connected) {
      scan_counter++;
    } else {
      scan_counter = 0;
    }

    // Check for WiFi config from queue
    if (g_wifi_config_queue != NULL) {
      if (xQueueReceive(g_wifi_config_queue, &wifi_cfg, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        // Validate SSID and password lengths
        int ssid_len = strlen(wifi_cfg.ssid);
        int pass_len = strlen(wifi_cfg.password);

        if (ssid_len > 0 && ssid_len < 33 && pass_len >= 0 && pass_len < 65) {
          strncpy(s_wifi_ssid, wifi_cfg.ssid, sizeof(s_wifi_ssid) - 1);
          s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
          strncpy(s_wifi_pass, wifi_cfg.password, sizeof(s_wifi_pass) - 1);
          s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';

          ESP_LOGI(TAG, "Received config from queue: SSID='%s', PASS='%s'",
                   s_wifi_ssid, s_wifi_pass);

          // Prepare new configuration
          memset(&s_pending_config, 0, sizeof(wifi_config_t));
          strncpy((char *)s_pending_config.sta.ssid, s_wifi_ssid,
                  sizeof(s_pending_config.sta.ssid));
          strncpy((char *)s_pending_config.sta.password, s_wifi_pass,
                  sizeof(s_pending_config.sta.password));
          s_pending_config.sta.threshold.authmode =
              ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
          s_pending_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
          strcpy((char *)s_pending_config.sta.sae_h2e_identifier,
                 EXAMPLE_H2E_IDENTIFIER);
#ifdef ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
          s_pending_config.sta.disable_wpa3_compatible_mode = 0;
#endif

          // Set reconnection flag and trigger disconnect
           s_reconnect_request = 1;
          if (s_wifi_connected) {
            esp_err_t ret = esp_wifi_disconnect();

            if (ret == ESP_ERR_WIFI_NOT_STARTED) {
              ESP_LOGE(TAG, "WiFi not started");
            } else if (ret != ESP_OK) {
              ESP_LOGE(TAG, "Disconnect failed: 0x%x", ret);
            }
          } else {
            esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL, 0,
                           portMAX_DELAY);
          }
      } else {
        ESP_LOGW(TAG, "Invalid SSID/Password length from queue");
      }
    }
  }
  else {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

ESP_LOGI(TAG, "WiFi config task exiting.");
vTaskDelete(NULL);
}

void wifi_connect_task_start(void) {
  wifi_connect_task_close = false;
  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (initial connection)");
  wifi_init_sta(s_wifi_ssid, s_wifi_pass);
  // Start the config task to listen for WiFi credentials from queue
  xTaskCreate(wifi_config_task, "wifi_config_task", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "WiFi config task created");
}

void wifi_connect_task_stop(void) {
  wifi_connect_task_close = true;
  ESP_LOGI(TAG, "Stopping WiFi connection task");
  ESP_ERROR_CHECK(esp_wifi_stop());
  ESP_ERROR_CHECK(esp_wifi_deinit());
  ESP_ERROR_CHECK(esp_event_loop_delete_default());
}
