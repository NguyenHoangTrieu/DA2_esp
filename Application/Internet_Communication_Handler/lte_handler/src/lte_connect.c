/**
 * LTE Connection Handler Implementation
 */

#include "lte_connect.h"
#include "config_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lte_handler.h"
#include <string.h>

/* Default LTE configuration */
#define DEFAULT_LTE_APN "v-internet"   // Viettel Vietnam
#define DEFAULT_LTE_USERNAME ""        // Leave empty if not required
#define DEFAULT_LTE_PASSWORD ""        // Leave empty if not required
#define LTE_RECONNECT_TIMEOUT_MS 30000 // 30 seconds
#define LTE_MAX_RECONNECT_ATTEMPTS 0   // 0 = infinite

static const char *TAG = "lte_connect";

/* Task control */
static bool lte_connect_task_close = false;
static TaskHandle_t lte_config_task_handle = NULL;

/* Current LTE configuration */
static char s_lte_apn[64] = DEFAULT_LTE_APN;
static char s_lte_username[32] = DEFAULT_LTE_USERNAME;
static char s_lte_password[32] = DEFAULT_LTE_PASSWORD;

/* Connection status */
static bool s_lte_connected = false;
static bool s_lte_initialized = false;

/* Event group for LTE status */
static EventGroupHandle_t s_lte_event_group = NULL;
#define LTE_CONNECTED_BIT BIT0
#define LTE_FAIL_BIT BIT1

/**
 * @brief LTE Handler event callback
 *
 * Receives events from LTE Handler middleware and updates local status
 */
static void lte_event_callback(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base != LTE_HANDLER_EVENT) {
    return;
  }

  switch (event_id) {
  case LTE_EVENT_INITIALIZED:
    ESP_LOGI(TAG, "LTE Handler initialized");
    s_lte_initialized = true;
    break;

  case LTE_EVENT_CONNECTING:
    ESP_LOGI(TAG, "LTE connecting...");
    s_lte_connected = false;
    break;

  case LTE_EVENT_CONNECTED:
    ESP_LOGI(TAG, "LTE connected successfully!");
    s_lte_connected = true;
    if (s_lte_event_group) {
      xEventGroupSetBits(s_lte_event_group, LTE_CONNECTED_BIT);
    }

    /* Log network info */
    lte_network_info_t net_info;
    if (lte_handler_get_ip_info(&net_info) == ESP_OK) {
      ESP_LOGI(TAG, "IP Address: %s", net_info.ip);
      ESP_LOGI(TAG, "Gateway: %s", net_info.gateway);
      ESP_LOGI(TAG, "DNS1: %s", net_info.dns1);
    }
    break;

  case LTE_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "LTE disconnected");
    s_lte_connected = false;
    if (s_lte_event_group) {
      xEventGroupSetBits(s_lte_event_group, LTE_FAIL_BIT);
    }
    break;

  case LTE_EVENT_RECONNECTING:
    ESP_LOGI(TAG, "LTE auto-reconnecting...");
    break;

  case LTE_EVENT_ERROR:
    ESP_LOGE(TAG, "LTE error occurred");
    s_lte_connected = false;
    if (s_lte_event_group) {
      xEventGroupSetBits(s_lte_event_group, LTE_FAIL_BIT);
    }
    break;

  default:
    break;
  }
}

/**
 * @brief Initialize LTE Handler with current configuration
 */
static esp_err_t lte_init_with_config(const char *apn, const char *username,
                                      const char *password) {
  if (s_lte_initialized) {
    ESP_LOGW(TAG, "LTE Handler already initialized, deinitializing first...");
    lte_handler_deinit();
    s_lte_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  /* Prepare configuration */
  lte_handler_config_t lte_config = LTE_HANDLER_CONFIG_DEFAULT();
  lte_config.apn = apn;

  /* Set username and password if provided */
  if (username && strlen(username) > 0) {
    lte_config.username = username;
  }
  if (password && strlen(password) > 0) {
    lte_config.password = password;
  }

  lte_config.auto_reconnect = true;
  lte_config.reconnect_timeout_ms = LTE_RECONNECT_TIMEOUT_MS;
  lte_config.max_reconnect_attempts = LTE_MAX_RECONNECT_ATTEMPTS;

  ESP_LOGI(TAG, "Initializing LTE Handler with APN: %s", apn);

  /* Initialize LTE Handler */
  esp_err_t ret = lte_handler_init(&lte_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize LTE Handler: 0x%x", ret);
    return ret;
  }

  s_lte_initialized = true;
  ESP_LOGI(TAG, "LTE Handler initialized successfully");

  /* Attempt connection */
  ret = lte_handler_connect();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start LTE connection: 0x%x", ret);
    return ret;
  }

  ESP_LOGI(TAG, "LTE connection initiated, waiting for connection...");
  return ESP_OK;
}

/**
 * @brief LTE config task
 *
 * Listens for LTE configuration from config_handler queue
 * and reconfigures connection when received
 */
static void lte_config_task(void *arg) {
  ESP_LOGI(TAG, "LTE config task started, listening for config from queue");

  lte_config_data_t lte_cfg;
  uint32_t monitor_counter = 0;

  while (!lte_connect_task_close) {
    /* Periodic signal quality monitoring */
    if (s_lte_connected && (monitor_counter++ % 50 == 0)) {
      uint32_t rssi, ber;
      if (lte_handler_get_signal_strength(&rssi, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Signal Quality - RSSI: %lu, BER: %lu", rssi, ber);
      }
    }

    /* Check for LTE config from queue */
    if (g_lte_config_queue != NULL) {
      if (xQueueReceive(g_lte_config_queue, &lte_cfg, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        /* Validate APN length */
        int apn_len = strlen(lte_cfg.apn);
        if (apn_len > 0 && apn_len < sizeof(s_lte_apn)) {
          /* Copy new configuration */
          strncpy(s_lte_apn, lte_cfg.apn, sizeof(s_lte_apn) - 1);
          s_lte_apn[sizeof(s_lte_apn) - 1] = '\0';

          if (lte_cfg.username[0] != '\0') {
            strncpy(s_lte_username, lte_cfg.username,
                    sizeof(s_lte_username) - 1);
            s_lte_username[sizeof(s_lte_username) - 1] = '\0';
          }

          if (lte_cfg.password[0] != '\0') {
            strncpy(s_lte_password, lte_cfg.password,
                    sizeof(s_lte_password) - 1);
            s_lte_password[sizeof(s_lte_password) - 1] = '\0';
          }

          ESP_LOGI(TAG, "Received new LTE config from queue:");
          ESP_LOGI(TAG, "  APN: '%s'", s_lte_apn);
          ESP_LOGI(TAG, "  Username: '%s'", s_lte_username);

          /* Disconnect current connection if connected */
          if (s_lte_connected) {
            ESP_LOGI(TAG, "Disconnecting current LTE connection...");
            lte_handler_disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
          }

          /* Reinitialize with new config */
          ESP_LOGI(TAG, "Reinitializing with new configuration...");
          esp_err_t ret =
              lte_init_with_config(s_lte_apn, s_lte_username, s_lte_password);
          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize LTE with new config");
          }
        } else {
          ESP_LOGW(TAG, "Invalid APN length from queue: %d", apn_len);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "LTE config task exiting");
  vTaskDelete(NULL);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */

void lte_connect_task_start(void) {
  if (lte_connect_task_close == false && lte_config_task_handle != NULL) {
    ESP_LOGW(TAG, "LTE connect task already running");
    return;
  }

  lte_connect_task_close = false;

  /* Create event group */
  if (s_lte_event_group == NULL) {
    s_lte_event_group = xEventGroupCreate();
  }

  /* Register event handler for LTE Handler events */
  esp_err_t ret = esp_event_handler_register(
      LTE_HANDLER_EVENT, ESP_EVENT_ANY_ID, &lte_event_callback, NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register LTE event handler: 0x%x", ret);
    return;
  }

  ESP_LOGI(TAG, "Starting LTE connection (initial)...");

  /* Initialize with default/stored config */
  ret = lte_init_with_config(s_lte_apn, s_lte_username, s_lte_password);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start LTE connection");
    return;
  }

  /* Create config task to listen for updates */
  BaseType_t task_ret = xTaskCreate(lte_config_task, "lte_config_task", 4096,
                                    NULL, 5, &lte_config_task_handle);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create LTE config task");
    return;
  }

  ESP_LOGI(TAG, "LTE config task created successfully");
}

void lte_connect_task_stop(void) {
  ESP_LOGI(TAG, "Stopping LTE connection task...");

  lte_connect_task_close = true;

  /* Wait for task to finish */
  if (lte_config_task_handle) {
    vTaskDelay(pdMS_TO_TICKS(200));
    lte_config_task_handle = NULL;
  }

  /* Disconnect and deinitialize */
  if (s_lte_initialized) {
    if (s_lte_connected) {
      lte_handler_disconnect();
    }
    lte_handler_deinit();
    s_lte_initialized = false;
  }

  /* Unregister event handler */
  esp_event_handler_unregister(LTE_HANDLER_EVENT, ESP_EVENT_ANY_ID,
                               &lte_event_callback);

  /* Delete event group */
  if (s_lte_event_group) {
    vEventGroupDelete(s_lte_event_group);
    s_lte_event_group = NULL;
  }

  s_lte_connected = false;
  ESP_LOGI(TAG, "LTE connection task stopped");
}

bool lte_is_connected(void) { return s_lte_connected; }

esp_err_t lte_get_signal_strength(uint32_t *rssi, uint32_t *ber) {
  if (!s_lte_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  return lte_handler_get_signal_strength(rssi, ber);
}
