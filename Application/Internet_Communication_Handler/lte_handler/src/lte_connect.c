/**
 * @file lte_connect.c
 * @brief LTE connection manager with config handler integration
 */

#include "lte_connect.h"
#include "config_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lte_handler.h"
#include <string.h>

/* ==================== Default Configuration ==================== */
#define LTE_DEFAULT_COMM_TYPE LTE_HANDLER_USB
#define LTE_DEFAULT_APN "v-internet"
#define LTE_AUTO_RECONNECT true
#define LTE_RECONNECT_TIMEOUT_MS 30000
#define LTE_MAX_RECONNECT 0 /* Infinite */

static const char *TAG = "LTE_CONNECT";

/* Internal state */
static struct {
  bool initialized;
  bool task_running;
  TaskHandle_t task_handle;
  char apn[64];
  char username[32];
  char password[32];
  lte_handler_comm_type_t comm_type;
} s_ctx = {.comm_type = LTE_DEFAULT_COMM_TYPE,
           .apn = LTE_DEFAULT_APN,
           .username = "",
           .password = ""};

/**
 * @brief Initialize/Reinitialize LTE with current config
 */
static esp_err_t lte_init_with_config(void) {
  if (s_ctx.initialized) {
    ESP_LOGI(TAG, "Reinitializing LTE...");
    lte_handler_disconnect();
    lte_handler_deinit();
    vTaskDelay(pdMS_TO_TICKS(1000));
    s_ctx.initialized = false;
  }

  lte_handler_config_t cfg = {
      .comm_type = s_ctx.comm_type,
      .apn = s_ctx.apn,
      .username = s_ctx.username[0] ? s_ctx.username : NULL,
      .password = s_ctx.password[0] ? s_ctx.password : NULL,
      .auto_reconnect = LTE_AUTO_RECONNECT,
      .reconnect_timeout_ms = LTE_RECONNECT_TIMEOUT_MS,
      .max_reconnect_attempts = LTE_MAX_RECONNECT};

  ESP_LOGI(TAG, "Initializing LTE - APN: %s, Type: %s", cfg.apn,
           cfg.comm_type == LTE_HANDLER_UART ? "UART" : "USB");

  esp_err_t ret = lte_handler_init(&cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init: 0x%x", ret);
    return ret;
  }

  s_ctx.initialized = true;

  ret = lte_handler_connect();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
    /* Don't fail - auto-reconnect will handle */
  }

  return ret;
}

/**
 * @brief Combined task: monitor connection + handle config updates
 */
static void lte_task(void *arg) {
  ESP_LOGI(TAG, "LTE task started");

  TickType_t last_monitor = 0;
  const TickType_t monitor_interval = pdMS_TO_TICKS(10000);

  /* Initial connection */
  lte_init_with_config();

  while (s_ctx.task_running) {
    /* Check for config updates from config_handler */
    lte_config_data_t new_cfg;
    if (g_lte_config_queue && xQueueReceive(g_lte_config_queue, &new_cfg,
                                            pdMS_TO_TICKS(100)) == pdTRUE) {

      ESP_LOGI(TAG, "Received new LTE config");
      ESP_LOGI(TAG, "APN: %s, Username: %s", new_cfg.apn, new_cfg.username);

      /* Update internal config */
      strncpy(s_ctx.apn, new_cfg.apn, sizeof(s_ctx.apn) - 1);
      strncpy(s_ctx.username, new_cfg.username, sizeof(s_ctx.username) - 1);
      strncpy(s_ctx.password, new_cfg.password, sizeof(s_ctx.password) - 1);

      /* Reinitialize with new config */
      lte_init_with_config();
    }

    /* Periodic monitoring */
    TickType_t now = xTaskGetTickCount();
    if ((now - last_monitor) >= monitor_interval) {
      if (s_ctx.initialized) {
        if (lte_handler_is_connected()) {
          uint32_t rssi, ber;
          if (lte_handler_get_signal_strength(&rssi, &ber) == ESP_OK) {
            ESP_LOGI(TAG, "Connected - RSSI: %lu, BER: %lu", rssi, ber);
          }
        } else {
          ESP_LOGW(TAG, "Not connected - State: %d", lte_handler_get_state());
        }
      }
      last_monitor = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }

  ESP_LOGI(TAG, "LTE task stopped");
  s_ctx.task_handle = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief Start LTE connection
 */
void lte_connect_task_start(void) {
  if (s_ctx.task_running) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  ESP_LOGI(TAG, "Starting LTE connect...");

  s_ctx.task_running = true;
  s_ctx.initialized = false;

  BaseType_t ret =
      xTaskCreate(lte_task, "lte_task", 4096, NULL, 5, &s_ctx.task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    s_ctx.task_running = false;
    return;
  }

  ESP_LOGI(TAG, "LTE connect started");
}

/**
 * @brief Stop LTE connection
 */
void lte_connect_task_stop(void) {
  if (!s_ctx.task_running) {
    ESP_LOGW(TAG, "Not running");
    return;
  }

  ESP_LOGI(TAG, "Stopping LTE connect...");

  s_ctx.task_running = false;

  /* Wait for task to exit */
  if (s_ctx.task_handle) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_ctx.task_handle = NULL;
  }

  /* Cleanup */
  if (s_ctx.initialized) {
    if (lte_handler_is_connected()) {
      lte_handler_disconnect();
    }
    lte_handler_deinit();
    s_ctx.initialized = false;
  }

  ESP_LOGI(TAG, "LTE connect stopped");
}
