/*
 * WiFi Connect Handler for ESP32-S3
 * Supports both Personal (WPA2/WPA3) and Enterprise (WPA2-Enterprise) modes
 * Using new esp_eap_client API (esp_wpa2 is deprecated)
 */

#include "wifi_connect.h"
#include "config_handler.h"
#include "esp_sntp.h"
#include "oled_monitor_task.h"
#include "pcf8563_rtc.h"

#define DEFAULT_ESP_WIFI_SSID "Devil"      // Initial hardcoded SSID
#define DEFAULT_ESP_WIFI_PASS "hamhap7604" // Initial hardcoded password
#define DEFAULT_ESP_WIFI_USERNAME                                              \
  "" // Enterprise username (empty for Personal mode)
#define WIFI_ESP_MAXIMUM_RETRY 3

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

static const char *TAG = "wifi connect";
static int s_retry_num = 0;

// Configurable WiFi credentials
wifi_config_context_t g_wifi_ctx = {.ssid = DEFAULT_ESP_WIFI_SSID,
                                    .pass = DEFAULT_ESP_WIFI_PASS,
                                    .username = DEFAULT_ESP_WIFI_USERNAME,
                                    .auth_mode = WIFI_AUTH_MODE_PERSONAL};

static uint8_t s_wifi_connected = 0; // Connection status flag
static volatile uint8_t s_reconnect_request =
    0;                                       // Flag for reconnection request
static wifi_config_t s_pending_config = {0}; // Pending WiFi config
static bool wifi_connect_task_close = false;
static bool g_sntp_synced = false;

// Network interface handle (global)
esp_netif_t *g_wifi_netif = NULL;

// SNTP sync notification callback
static void sntp_sync_notification_cb(struct timeval *tv) {
  ESP_LOGI(TAG, "SNTP time synchronized!");
  g_sntp_synced = true;

  // Sync system time to PCF8563
  time_t now = tv->tv_sec;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  esp_err_t ret = pcf8563_write_time(&timeinfo);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "System time synced to PCF8563 RTC");
    // Clear voltage low flag after successful write
    pcf8563_clear_voltage_low_flag();
    // Ensure clock is running
    pcf8563_start();
  } else {
    ESP_LOGW(TAG, "Failed to sync time to PCF8563: %s", esp_err_to_name(ret));
  }
}

// Function to initialize SNTP
static void wifi_init_sntp(void) {
  ESP_LOGI(TAG, "Initializing SNTP");

  // Set timezone to Vietnam (GMT+7)
  setenv("TZ", "ICT-7", 1);
  tzset();

  // Initialize SNTP
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_setservername(2, "time.cloudflare.com");

  // Set notification callback
  esp_sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);

  // Set sync mode to smooth adjustment
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

  esp_sntp_init();

  ESP_LOGI(TAG, "SNTP initialized, waiting for sync...");
}

bool wifi_is_sntp_synced(void) { return g_sntp_synced; }

uint8_t wifi_get_connection_status(void) { return s_wifi_connected; }

/*
 * WiFi event handler monitors connection events, initiates reconnect,
 * and signals status in FreeRTOS event group.
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
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
    } else if (s_retry_num < WIFI_ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "Retrying to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      ESP_LOGI(TAG, "Connect to the AP failed");
      oled_monitor_update_wifi(false);
    }

    ESP_LOGI(TAG, "Disconnected from WiFi");
    s_wifi_connected = 0;
    oled_monitor_update_wifi(false);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_wifi_connected = 1;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    is_internet_connected = true;
    wifi_init_sntp();
    oled_monitor_update_wifi(true);
  }
}

/*
 * Configure WPA2-Enterprise authentication using new esp_eap_client API
 */
static esp_err_t wifi_configure_enterprise(const char *username,
                                           const char *password) {
  esp_err_t ret;

  ESP_LOGI(TAG, "Configuring WPA2-Enterprise with username: %s", username);

  // Set identity (used in EAP-Response/Identity)
  ret = esp_eap_client_set_identity((uint8_t *)username, strlen(username));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set identity: %s", esp_err_to_name(ret));
    return ret;
  }

  // Set username for Phase 2 (PEAP/TTLS)
  ret = esp_eap_client_set_username((uint8_t *)username, strlen(username));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set username: %s", esp_err_to_name(ret));
    return ret;
  }

  // Set password for Phase 2
  ret = esp_eap_client_set_password((uint8_t *)password, strlen(password));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set password: %s", esp_err_to_name(ret));
    return ret;
  }

  // Optional: Set TTLS Phase 2 method (default is typically MSCHAPv2)
  // Uncomment if you need PAP instead:
  // esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_PAP);

  // Optional: Use default certificate bundle for CA verification
  // Uncomment if needed for secure connections:
  // esp_eap_client_use_default_cert_bundle(true);

  // Enable WPA2-Enterprise
  ret = esp_wifi_sta_enterprise_enable();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable WPA2-Enterprise: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "WPA2-Enterprise configured successfully");
  return ESP_OK;
}

/*
 * Initialize WiFi in STA mode and connect.
 * Supports both Personal and Enterprise modes.
 */
void wifi_init_sta(const char *custom_ssid, const char *custom_pass,
                   const char *custom_username,
                   wifi_conf_auth_mode_t auth_mode) {
  s_wifi_event_group = xEventGroupCreate();

  if (auth_mode == WIFI_AUTH_MODE_ENTERPRISE) {
    ESP_LOGI(TAG, "CONNECTING TO WIFI (ENTERPRISE) SSID:%s USERNAME:%s",
             custom_ssid, custom_username);
  } else {
    ESP_LOGI(TAG, "CONNECTING TO WIFI (PERSONAL) SSID:%s PASSWORD:%s",
             custom_ssid, custom_pass);
  }

  g_wifi_netif = esp_netif_create_default_wifi_sta();

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

  if (auth_mode == WIFI_AUTH_MODE_ENTERPRISE) {
    // For Enterprise, don't set password in wifi_config
    // Password is set via esp_eap_client_set_password()
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
  } else {
    // For Personal mode
    strncpy((char *)wifi_config.sta.password, custom_pass,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
    strcpy((char *)wifi_config.sta.sae_h2e_identifier, EXAMPLE_H2E_IDENTIFIER);

#ifdef ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
    wifi_config.sta.disable_wpa3_compatible_mode = 0;
#endif
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // Configure Enterprise authentication if needed
  if (auth_mode == WIFI_AUTH_MODE_ENTERPRISE) {
    esp_err_t ret = wifi_configure_enterprise(custom_username, custom_pass);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to configure enterprise authentication");
      return;
    }
  }

  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "wifi_init_sta finished.");

  // Wait for either successful connection or maximum retry/failure
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_CONNECTED_BIT) {
    if (auth_mode == WIFI_AUTH_MODE_ENTERPRISE) {
      ESP_LOGI(TAG, "Connected to AP SSID:%s (Enterprise)", custom_ssid);
    } else {
      ESP_LOGI(TAG, "Connected to AP SSID:%s Password:%s", custom_ssid,
               custom_pass);
    }
    s_wifi_connected = true;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s", custom_ssid);
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
  wifi_config_data_t wifi_cfg;

  while (!wifi_connect_task_close) {
    // Check for WiFi config from queue
    if (g_wifi_config_queue != NULL) {
      if (xQueueReceive(g_wifi_config_queue, &wifi_cfg, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        // Validate SSID and password lengths
        int ssid_len = strlen(wifi_cfg.ssid);
        int pass_len = strlen(wifi_cfg.password);
        int username_len = strlen(wifi_cfg.username);

        if (ssid_len > 0 && ssid_len < 33 && pass_len >= 0 && pass_len < 65) {
          // Update credentials
          strncpy(g_wifi_ctx.ssid, wifi_cfg.ssid, sizeof(g_wifi_ctx.ssid) - 1);
          g_wifi_ctx.ssid[sizeof(g_wifi_ctx.ssid) - 1] = '\0';

          strncpy(g_wifi_ctx.pass, wifi_cfg.password,
                  sizeof(g_wifi_ctx.pass) - 1);
          g_wifi_ctx.pass[sizeof(g_wifi_ctx.pass) - 1] = '\0';

          // Determine authentication mode
          if (username_len > 0 && username_len < 65) {
            // Enterprise mode
            strncpy(g_wifi_ctx.username, wifi_cfg.username,
                    sizeof(g_wifi_ctx.username) - 1);
            g_wifi_ctx.username[sizeof(g_wifi_ctx.username) - 1] = '\0';
            g_wifi_ctx.auth_mode = WIFI_AUTH_MODE_ENTERPRISE;

            ESP_LOGI(TAG,
                     "Received Enterprise config from queue: "
                     "SSID='%s', USERNAME='%s'",
                     g_wifi_ctx.ssid, g_wifi_ctx.username);
          } else {
            // Personal mode
            g_wifi_ctx.auth_mode = WIFI_AUTH_MODE_PERSONAL;
            ESP_LOGI(TAG,
                     "Received Personal config from queue: "
                     "SSID='%s', PASS='%s'",
                     g_wifi_ctx.ssid, g_wifi_ctx.pass);
          }

          // Prepare new configuration
          memset(&s_pending_config, 0, sizeof(wifi_config_t));
          strncpy((char *)s_pending_config.sta.ssid, g_wifi_ctx.ssid,
                  sizeof(s_pending_config.sta.ssid));

          if (g_wifi_ctx.auth_mode == WIFI_AUTH_MODE_ENTERPRISE) {
            s_pending_config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;

            // Configure enterprise settings before reconnection
            wifi_configure_enterprise(g_wifi_ctx.username, g_wifi_ctx.pass);
          } else {
            strncpy((char *)s_pending_config.sta.password, g_wifi_ctx.pass,
                    sizeof(s_pending_config.sta.password));
            s_pending_config.sta.threshold.authmode =
                ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
            s_pending_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
            strcpy((char *)s_pending_config.sta.sae_h2e_identifier,
                   EXAMPLE_H2E_IDENTIFIER);

#ifdef ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
            s_pending_config.sta.disable_wpa3_compatible_mode = 0;
#endif
            // Disable enterprise mode when switching to Personal
            esp_wifi_sta_enterprise_disable();
          }

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
            // If not connected, directly post disconnect event
            esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL, 0,
                           portMAX_DELAY);
          }
        } else {
          ESP_LOGW(TAG, "Invalid SSID/Password/Username length from queue");
        }
        save_wifi_config_to_nvs();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "WiFi config task exiting.");
  vTaskDelete(NULL);
}

void wifi_connect_task_start(void) {
  wifi_connect_task_close = false;

  // Determine initial mode
  if (strlen(g_wifi_ctx.username) > 0) {
    g_wifi_ctx.auth_mode = WIFI_AUTH_MODE_ENTERPRISE;
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (Enterprise initial connection)");
    wifi_init_sta(g_wifi_ctx.ssid, g_wifi_ctx.pass, g_wifi_ctx.username,
                  WIFI_AUTH_MODE_ENTERPRISE);
  } else {
    g_wifi_ctx.auth_mode = WIFI_AUTH_MODE_PERSONAL;
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA (Personal initial connection)");
    wifi_init_sta(g_wifi_ctx.ssid, g_wifi_ctx.pass, "",
                  WIFI_AUTH_MODE_PERSONAL);
  }

  // Start the config task to listen for WiFi credentials from queue
  xTaskCreate(wifi_config_task, "wifi_config_task", 8192, NULL, 5, NULL);
  ESP_LOGI(TAG, "WiFi config task created");
}

void wifi_connect_task_stop(void) {
  if (wifi_connect_task_close) {
    return;
  }

  wifi_connect_task_close = true;
  ESP_LOGI(TAG, "Stopping WiFi connection task");

  // Disable enterprise mode if enabled
  esp_wifi_sta_enterprise_disable();

  ESP_ERROR_CHECK(esp_wifi_stop());
  ESP_ERROR_CHECK(esp_wifi_deinit());

  esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler);
  esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler);

  if (g_wifi_netif) {
    esp_netif_destroy(g_wifi_netif);
    g_wifi_netif = NULL;
  }

  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
  }
}
