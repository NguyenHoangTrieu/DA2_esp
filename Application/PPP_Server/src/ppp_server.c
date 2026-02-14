/*
 * PPP Server Component for WAN MCU (Using eppp_link)
 */

#include "ppp_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ppp_server_config.h"

// eppp_link API
#include "eppp_link.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ppp_server";

/* PPP Server State */
static esp_netif_t *s_ppp_netif = NULL;
static bool s_ppp_server_initialized = false;
static TaskHandle_t s_ppp_task_handle = NULL;

/* Event group for PPP connection status */
static EventGroupHandle_t s_ppp_event_group = NULL;
#define PPP_CLIENT_CONNECTED_BIT BIT0

/* eppp_link configuration */
static eppp_config_t s_ppp_config = EPPP_DEFAULT_SERVER_CONFIG();;

/**
 * @brief PPP Server event handler
 */
static void ppp_server_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      ESP_LOGI(
          TAG,
          "================================================================");
      ESP_LOGI(TAG, "PPP Client Connected!");
      ESP_LOGI(TAG, "Client IP  : " IPSTR, IP2STR(&event->ip_info.ip));
      ESP_LOGI(TAG, "Netmask    : " IPSTR, IP2STR(&event->ip_info.netmask));
      ESP_LOGI(TAG, "Gateway    : " IPSTR, IP2STR(&event->ip_info.gw));
      ESP_LOGI(
          TAG,
          "================================================================");

      if (s_ppp_event_group) {
        xEventGroupSetBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
      }
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
      ESP_LOGW(TAG, "PPP Client Disconnected");
      if (s_ppp_event_group) {
        xEventGroupClearBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
      }
    }
  }
}

/**
 * @brief PPP server task using eppp_listen() blocking API
 */
static void ppp_server_task(void *pvParameters) {
  ESP_LOGI(TAG, "PPP server task started");
  ESP_LOGI(TAG, "Waiting for PPP client connection...");
  s_ppp_netif = eppp_listen(&s_ppp_config);

  if (s_ppp_netif) {
    ESP_LOGI(TAG, "PPP connection established successfully");
    ESP_LOGI(TAG, "NAPT enabled between WiFi and PPP interfaces");

    // Enable NAPT between WiFi and PPP interfaces
#if PPP_SERVER_INTERNET_EPPP_CHANNEL
    station_over_eppp_channel(eppp_netif);
#else
    ESP_ERROR_CHECK(esp_netif_napt_enable(s_ppp_netif));
#endif

    // Set connected bit
    if (s_ppp_event_group) {
      xEventGroupSetBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
    }

    // Keep task alive to maintain connection
    while (s_ppp_server_initialized) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } else {
    ESP_LOGE(TAG, "Failed to establish PPP connection");
  }

  ESP_LOGI(TAG, "PPP server task exiting");
  s_ppp_task_handle = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief Initialize the PPP server using eppp_link
 */
esp_err_t ppp_server_init() {
  if (s_ppp_server_initialized) {
    ESP_LOGW(TAG, "PPP server already initialized");
    return ESP_OK;
  }

  // Create event group for connection status
  s_ppp_event_group = xEventGroupCreate();
  if (!s_ppp_event_group) {
    ESP_LOGE(TAG, "Failed to create event group");
    return ESP_ERR_NO_MEM;
  }

  // Register event handler for PPP events
  esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                             &ppp_server_event_handler, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
    vEventGroupDelete(s_ppp_event_group);
    s_ppp_event_group = NULL;
    return err;
  }

  s_ppp_config.transport = EPPP_TRANSPORT_UART;
  s_ppp_config.uart.port = PPP_UART_PORT;
  s_ppp_config.uart.baud = PPP_UART_BAUD_RATE;
  s_ppp_config.uart.tx_io = PPP_UART_TX_PIN;
  s_ppp_config.uart.rx_io = PPP_UART_RX_PIN;
  s_ppp_config.uart.queue_size = PPP_UART_QUEUE_SIZE;
  s_ppp_config.uart.rx_buffer_size = PPP_UART_BUF_SIZE;

  // Create PPP server task
  BaseType_t task_created =
      xTaskCreate(ppp_server_task, "ppp_server", PPP_SERVER_TASK_STACK_SIZE,
                  NULL, PPP_SERVER_TASK_PRIORITY, &s_ppp_task_handle);

  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PPP server task");
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                 &ppp_server_event_handler);
    vEventGroupDelete(s_ppp_event_group);
    s_ppp_event_group = NULL;
    return ESP_FAIL;
  }

  s_ppp_server_initialized = true;

  return ESP_OK;
}

/**
 * @brief Deinitialize the PPP server
 */
esp_err_t ppp_server_deinit(void) {
  if (!s_ppp_server_initialized) {
    ESP_LOGW(TAG, "PPP server not initialized");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Deinitializing PPP server...");

  // Signal task to exit FIRST
  s_ppp_server_initialized = false;

  // Deinitialize eppp_link BEFORE waiting for task
  if (s_ppp_netif) {
    ESP_LOGI(TAG, "Calling eppp_deinit()...");
    eppp_deinit(s_ppp_netif);
    s_ppp_netif = NULL;
    ESP_LOGI(TAG, "eppp_deinit() completed");
  }

  // Give more time for cleanup
  vTaskDelay(pdMS_TO_TICKS(500));

  // Wait for task to finish
  if (s_ppp_task_handle) {
    ESP_LOGI(TAG, "Waiting for PPP task to exit...");
    for (int i = 0; i < 50; i++) {
      if (s_ppp_task_handle == NULL) {
        ESP_LOGI(TAG, "PPP task exited successfully");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_ppp_task_handle != NULL) {
      ESP_LOGW(TAG, "PPP task did not exit, force deleting...");
      vTaskDelete(s_ppp_task_handle);
      s_ppp_task_handle = NULL;
    }
  }

  // Unregister event handler
  esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                               &ppp_server_event_handler);

  // Cleanup event group
  if (s_ppp_event_group) {
    vEventGroupDelete(s_ppp_event_group);
    s_ppp_event_group = NULL;
  }

  ESP_LOGI(TAG, "PPP server deinitialized successfully");
  return ESP_OK;
}

/**
 * @brief Check if PPP client is connected
 */
bool ppp_server_is_client_connected(void) {
  if (!s_ppp_event_group) {
    return false;
  }

  EventBits_t bits = xEventGroupGetBits(s_ppp_event_group);
  return (bits & PPP_CLIENT_CONNECTED_BIT) != 0;
}

bool ppp_server_is_initialized(void) {
  return s_ppp_server_initialized;
}
