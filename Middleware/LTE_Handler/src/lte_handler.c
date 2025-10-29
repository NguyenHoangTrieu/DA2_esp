#include "lte_handler.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem.h"
#include "esp_modem_netif.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include specific modem drivers based on configuration */
#if LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_SIM7600
#include "sim7600_comm.h"
#define MODEM_INIT_FUNC sim7600_init
#elif LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_BG96
#include "bg96_comm.h"
#define MODEM_INIT_FUNC bg96_init
#elif LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_SIM800
#include "sim800_comm.h"
#define MODEM_INIT_FUNC sim800_init
#else
#error "Unsupported modem device"
#endif

#define SIGNAL_POLL_INTERVAL_MS 10000
#define CONNECT_BIT BIT0
#define DISCONNECT_BIT BIT1

static const char *TAG = "LTE_HANDLER";

typedef struct {
  lte_handler_config_t config;
  lte_handler_state_t state;
  modem_dte_t *dte;
  modem_dce_t *dce;
  esp_netif_t *esp_netif;
  void *modem_netif_adapter;
  lte_modem_info_t modem_info;
  lte_network_info_t network_info;
  bool modem_info_valid;
  bool network_info_valid;
  SemaphoreHandle_t mutex;
  uint32_t reconnect_attempts;
  TaskHandle_t bg_task;
  EventGroupHandle_t event_group;
  bool initialized;
} lte_handler_ctx_t;

static lte_handler_ctx_t *ctx = NULL;

/**
 * @brief Internal: Set LTE Handler state with thread safety and logging
 */
static void set_state(lte_handler_state_t new_state) {
  if (ctx && ctx->mutex) {
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "State: %d -> %d", ctx->state, new_state);
    ctx->state = new_state;
    xSemaphoreGive(ctx->mutex);
  } else if (ctx) {
    ESP_LOGI(TAG, "State: %d -> %d", ctx->state, new_state);
    ctx->state = new_state;
  }
}

/**
 * @brief Modem event handler
 */
static void modem_event_handler(void *event_handler_arg,
                                esp_event_base_t event_base, int32_t event_id,
                                void *event_data) {
  switch (event_id) {
  case ESP_MODEM_EVENT_PPP_START:
    ESP_LOGI(TAG, "Modem PPP Started");
    break;
  case ESP_MODEM_EVENT_PPP_STOP:
    ESP_LOGI(TAG, "Modem PPP Stopped");
    if (ctx && ctx->event_group) {
      xEventGroupSetBits(ctx->event_group, DISCONNECT_BIT);
    }
    set_state(LTE_STATE_DISCONNECTED);
    break;
  case ESP_MODEM_EVENT_UNKNOWN:
    ESP_LOGW(TAG, "Unknown line received: %s", (char *)event_data);
    break;
  default:
    break;
  }
}

/**
 * @brief IP event handler
 */
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
  ESP_LOGD(TAG, "IP event! %d", event_id);

  if (event_id == IP_EVENT_PPP_GOT_IP) {
    esp_netif_dns_info_t dns_info;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    esp_netif_t *netif = event->esp_netif;

    ESP_LOGI(TAG, "Modem Connect to PPP Server");
    ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
    ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));

    if (ctx) {
      // Store network info
      snprintf(ctx->network_info.ip, sizeof(ctx->network_info.ip), IPSTR,
               IP2STR(&event->ip_info.ip));
      snprintf(ctx->network_info.netmask, sizeof(ctx->network_info.netmask),
               IPSTR, IP2STR(&event->ip_info.netmask));
      snprintf(ctx->network_info.gateway, sizeof(ctx->network_info.gateway),
               IPSTR, IP2STR(&event->ip_info.gw));

      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
      snprintf(ctx->network_info.dns1, sizeof(ctx->network_info.dns1), IPSTR,
               IP2STR(&dns_info.ip.u_addr.ip4));
      ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));

      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
      snprintf(ctx->network_info.dns2, sizeof(ctx->network_info.dns2), IPSTR,
               IP2STR(&dns_info.ip.u_addr.ip4));
      ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));

      ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
      ctx->network_info_valid = true;
      set_state(LTE_STATE_CONNECTED);

      if (ctx->event_group) {
        xEventGroupSetBits(ctx->event_group, CONNECT_BIT);
      }
    }
  } else if (event_id == IP_EVENT_PPP_LOST_IP) {
    ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
    if (ctx) {
      ctx->network_info_valid = false;
      set_state(LTE_STATE_DISCONNECTED);
    }
  } else if (event_id == IP_EVENT_GOT_IP6) {
    ESP_LOGI(TAG, "GOT IPv6 event!");
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
  }
}

/**
 * @brief PPP status event handler
 */
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
  ESP_LOGI(TAG, "PPP state changed event %d", event_id);
  if (event_id == NETIF_PPP_ERRORUSER) {
    esp_netif_t *netif = event_data;
    ESP_LOGI(TAG, "User interrupted event from netif:%p", netif);
  }
}

/**
 * @brief Internal: Initialize esp_modem driver and DTE/DCE
 */
static esp_err_t modem_init(void) {
  ESP_LOGI(TAG, "Modem DTE init...");

  esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
  ctx->dte = esp_modem_dte_init(&dte_cfg);
  if (!ctx->dte) {
    ESP_LOGE(TAG, "esp_modem_dte_init fail");
    return ESP_FAIL;
  }

  // Register event handler for modem events
  ESP_ERROR_CHECK(esp_modem_set_event_handler(modem_event_handler, ESP_EVENT_ANY_ID, NULL));

  ESP_LOGI(TAG, "DCE init...");
  ctx->dce = MODEM_INIT_FUNC(ctx->dte);
  if (!ctx->dce) {
    ESP_LOGE(TAG, "MODEM_INIT_FUNC fail");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Set flow ctrl NONE");
  ESP_ERROR_CHECK(ctx->dce->set_flow_ctrl(ctx->dce, MODEM_FLOW_CONTROL_NONE));
  ESP_ERROR_CHECK(ctx->dce->store_profile(ctx->dce));

  // Query modem info
  strncpy(ctx->modem_info.module_name, ctx->dce->name,
          sizeof(ctx->modem_info.module_name) - 1);
  strncpy(ctx->modem_info.operator_name, ctx->dce->oper,
          sizeof(ctx->modem_info.operator_name) - 1);
  strncpy(ctx->modem_info.imei, ctx->dce->imei,
          sizeof(ctx->modem_info.imei) - 1);
  strncpy(ctx->modem_info.imsi, ctx->dce->imsi,
          sizeof(ctx->modem_info.imsi) - 1);

  // Get initial signal quality
  uint32_t rssi = 0, ber = 0;
  if (ctx->dce->get_signal_quality(ctx->dce, &rssi, &ber) == ESP_OK) {
    ctx->modem_info.rssi = rssi;
    ctx->modem_info.ber = ber;
    ESP_LOGI(TAG, "Signal quality: RSSI=%lu, BER=%lu", rssi, ber);
  }

  // Get battery status if available
  uint32_t voltage = 0, bcs = 0, bcl = 0;
  if (ctx->dce->get_battery_status &&
      ctx->dce->get_battery_status(ctx->dce, &bcs, &bcl, &voltage) == ESP_OK) {
    ESP_LOGI(TAG, "Battery voltage: %lu mV", voltage);
  }

  ctx->modem_info_valid = true;

  ESP_LOGI(TAG, "Modem ready");
  ESP_LOGI(TAG, "Module: %s", ctx->dce->name);
  ESP_LOGI(TAG, "Operator: %s", ctx->dce->oper);
  ESP_LOGI(TAG, "IMEI: %s", ctx->dce->imei);
  ESP_LOGI(TAG, "IMSI: %s", ctx->dce->imsi);

  return ESP_OK;
}

/**
 * @brief Internal: Background task for periodic signal polling and reconnect
 * management
 */
static void lte_handler_bg_task(void *param) {
  ESP_LOGI(TAG, "Background LTE task started");

  while (ctx && ctx->initialized) {
    if (ctx->state == LTE_STATE_CONNECTED && ctx->dce) {
      uint32_t rssi, ber;
      if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (ctx->dce->get_signal_quality(ctx->dce, &rssi, &ber) == ESP_OK) {
          ctx->modem_info.rssi = rssi;
          ctx->modem_info.ber = ber;
          ESP_LOGI(TAG, "Signal RSSI=%lu BER=%lu", rssi, ber);
        }
        xSemaphoreGive(ctx->mutex);
      }
    }

    if (ctx->config.auto_reconnect && ctx->state == LTE_STATE_DISCONNECTED &&
        (ctx->config.max_reconnect_attempts == 0 ||
         ctx->reconnect_attempts < ctx->config.max_reconnect_attempts)) {
      ctx->reconnect_attempts++;
      ESP_LOGW(TAG, "Auto reconnect, attempt %lu", ctx->reconnect_attempts);
      set_state(LTE_STATE_RECONNECTING);
      vTaskDelay(pdMS_TO_TICKS(ctx->config.reconnect_timeout_ms));
      lte_handler_connect();
    }

    vTaskDelay(pdMS_TO_TICKS(SIGNAL_POLL_INTERVAL_MS));
  }

  ESP_LOGI(TAG, "Background LTE task stopped");
  vTaskDelete(NULL);
}

/**
 * @brief Initialize the LTE subsystem, create context and bg task
 */
esp_err_t lte_handler_init(const lte_handler_config_t *config) {
  if (!config || !config->apn) {
    ESP_LOGE(TAG, "Init failed, no config or APN");
    return ESP_ERR_INVALID_ARG;
  }

  if (ctx) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  ctx = calloc(1, sizeof(lte_handler_ctx_t));
  if (!ctx) {
    ESP_LOGE(TAG, "Alloc fail");
    return ESP_ERR_NO_MEM;
  }

  memcpy(&ctx->config, config, sizeof(lte_handler_config_t));
  ctx->mutex = xSemaphoreCreateMutex();
  ctx->event_group = xEventGroupCreate();

  if (!ctx->mutex || !ctx->event_group) {
    ESP_LOGE(TAG, "Failed to create mutex or event group");
    lte_handler_deinit();
    return ESP_ERR_NO_MEM;
  }

  set_state(LTE_STATE_INITIALIZING);
  ctx->initialized = true;
  ctx->reconnect_attempts = 0;
  // ESP_ERROR_CHECK(esp_event_loop_create_default());
  // Initialize ESP netif and register event handlers
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                             &on_ip_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                             &on_ppp_changed, NULL));

  // Initialize modem
  if (modem_init() != ESP_OK) {
    ESP_LOGE(TAG, "Modem init failed");
    lte_handler_deinit();
    return ESP_FAIL;
  }

  // Create netif for PPP
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
  ctx->esp_netif = esp_netif_new(&cfg);
  if (!ctx->esp_netif) {
    ESP_LOGE(TAG, "Failed to create netif");
    lte_handler_deinit();
    return ESP_FAIL;
  }

  // Setup modem netif adapter
  ctx->modem_netif_adapter = esp_modem_netif_setup(ctx->dte);
  esp_modem_netif_set_default_handlers(ctx->esp_netif);

  // Set authentication if configured
#if defined(CONFIG_LWIP_PPP_PAP_SUPPORT) ||                                    \
    defined(CONFIG_LWIP_PPP_CHAP_SUPPORT)
  if (ctx->config.username && ctx->config.password) {
#ifdef CONFIG_LWIP_PPP_PAP_SUPPORT
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_PAP;
#else
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_CHAP;
#endif
    esp_netif_ppp_set_auth(ctx->esp_netif, auth_type, ctx->config.username,
                           ctx->config.password);
  }
#endif

  set_state(LTE_STATE_INITIALIZED);

  // Create background task
  xTaskCreate(lte_handler_bg_task, "lte_handler_bg_task", 4096, NULL, 5,
              &ctx->bg_task);

  ESP_LOGI(TAG, "LTE handler fully initialized");
  return ESP_OK;
}

/**
 * @brief De-initialize and free all LTE resources and tasks
 */
esp_err_t lte_handler_deinit(void) {
  ESP_LOGI(TAG, "Deinit called");

  if (!ctx)
    return ESP_ERR_INVALID_STATE;

  ctx->initialized = false;
  vTaskDelay(pdMS_TO_TICKS(100));

  if (ctx->bg_task) {
    ctx->bg_task = NULL;
  }

  // Cleanup netif and modem adapter
  if (ctx->modem_netif_adapter) {
    esp_modem_netif_clear_default_handlers();
    esp_modem_netif_teardown(ctx->modem_netif_adapter);
    ctx->modem_netif_adapter = NULL;
  }

  if (ctx->esp_netif) {
    esp_netif_destroy(ctx->esp_netif);
    ctx->esp_netif = NULL;
  }

  if (ctx->dce) {
    ctx->dce->deinit(ctx->dce);
    ctx->dce = NULL;
  }

  if (ctx->dte) {
    ctx->dte->deinit(ctx->dte);
    ctx->dte = NULL;
  }
  // Unregister modem event handler
  esp_modem_remove_event_handler(modem_event_handler);

  // Unregister event handlers
  esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event);
  esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                               &on_ppp_changed);
  // esp_event_loop_delete_default();

  if (ctx->event_group)
    vEventGroupDelete(ctx->event_group);

  if (ctx->mutex)
    vSemaphoreDelete(ctx->mutex);

  free(ctx);
  ctx = NULL;

  ESP_LOGI(TAG, "LTE handler deinitialized");
  return ESP_OK;
}

/**
 * @brief Start PPP connection, update state, fill network info from events
 */
esp_err_t lte_handler_connect(void) {
  ESP_LOGI(TAG, "Starting PPP...");

  if (!ctx || !ctx->dte || !ctx->dce) {
    ESP_LOGE(TAG, "Connect fail: no context/dte/dce");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return ESP_ERR_TIMEOUT;
  }

  if (ctx->state == LTE_STATE_CONNECTED || ctx->state == LTE_STATE_CONNECTING) {
    ESP_LOGW(TAG, "Already connecting/connected");
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
  }

  // Define PDP context
  ESP_ERROR_CHECK(
      ctx->dce->define_pdp_context(ctx->dce, 1, "IP", ctx->config.apn));

  set_state(LTE_STATE_CONNECTING);
  xSemaphoreGive(ctx->mutex);

  // Attach modem to netif
  esp_netif_attach(ctx->esp_netif, ctx->modem_netif_adapter);

  // Start PPP mode
  esp_err_t err = esp_modem_start_ppp(ctx->dte);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start PPP: %d", err);
    set_state(LTE_STATE_ERROR);
    return err;
  }

  // Wait for connection with timeout
  EventBits_t bits = xEventGroupWaitBits(ctx->event_group, CONNECT_BIT, pdTRUE,
                                         pdTRUE, pdMS_TO_TICKS(30000));

  if (bits & CONNECT_BIT) {
    ESP_LOGI(TAG, "PPP connected successfully");
    ctx->reconnect_attempts = 0;
    return ESP_OK;
  } else {
    ESP_LOGE(TAG, "PPP connection timeout");
    set_state(LTE_STATE_ERROR);
    return ESP_ERR_TIMEOUT;
  }
}

/**
 * @brief Stop PPP connection, update state and network info
 */
esp_err_t lte_handler_disconnect(void) {
  ESP_LOGI(TAG, "Disconnect called");

  if (!ctx || !ctx->dte) {
    ESP_LOGE(TAG, "Disconnect fail: no context/dte");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return ESP_ERR_TIMEOUT;
  }

  if (ctx->state != LTE_STATE_CONNECTED) {
    ESP_LOGW(TAG, "Not connected");
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
  }

  xSemaphoreGive(ctx->mutex);

  esp_err_t ret = esp_modem_stop_ppp(ctx->dte);

  // Wait for disconnect event
  xEventGroupWaitBits(ctx->event_group, DISCONNECT_BIT, pdTRUE, pdTRUE,
                      pdMS_TO_TICKS(10000));

  set_state(LTE_STATE_DISCONNECTED);
  ctx->network_info_valid = false;

  ESP_LOGI(TAG, "PPP stopped, DISCONNECTED");
  return ret;
}

/**
 * @brief Get current LTE handler state with thread safety
 */
lte_handler_state_t lte_handler_get_state(void) {
  if (!ctx)
    return LTE_STATE_IDLE;

  lte_handler_state_t val;
  if (ctx->mutex) {
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    val = ctx->state;
    xSemaphoreGive(ctx->mutex);
  } else {
    val = ctx->state;
  }

  ESP_LOGD(TAG, "Get state: %d", val);
  return val;
}

/**
 * @brief Returns true if PPP is up and LTE is connected
 */
bool lte_handler_is_connected(void) {
  bool connected = (ctx && ctx->state == LTE_STATE_CONNECTED);
  ESP_LOGD(TAG, "Is connected: %d", connected);
  return connected;
}

/**
 * @brief Query and return signal quality (RSSI/BER) from modem
 */
esp_err_t lte_handler_get_signal_strength(uint32_t *rssi, uint32_t *ber) {
  if (!ctx || !rssi || !ber) {
    ESP_LOGE(TAG, "Get signal failed: null param");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->dce) {
    ESP_LOGE(TAG, "Get signal failed: dce not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to take mutex");
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t ret = ctx->dce->get_signal_quality(ctx->dce, rssi, ber);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Signal read: RSSI=%lu BER=%lu", *rssi, *ber);
    ctx->modem_info.rssi = *rssi;
    ctx->modem_info.ber = *ber;
    ctx->modem_info_valid = true;
  } else {
    ESP_LOGW(TAG, "Get signal failed");
  }

  xSemaphoreGive(ctx->mutex);
  return ret;
}

/**
 * @brief Return modem operator name (from context)
 */
esp_err_t lte_handler_get_operator_name(char *operator_name, size_t max_len) {
  if (!ctx || !operator_name || max_len == 0) {
    ESP_LOGE(TAG, "Get operator failed: bad args");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    ESP_LOGE(TAG, "Get operator failed: modem info not valid");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(operator_name, ctx->modem_info.operator_name, max_len - 1);
  operator_name[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Get operator: %s", operator_name);
  return ESP_OK;
}

/**
 * @brief Get network info (IP address, gateway, DNS, etc.)
 */
esp_err_t lte_handler_get_ip_info(lte_network_info_t *info) {
  if (!ctx || !info) {
    ESP_LOGE(TAG, "Get IP info failed: bad args");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->network_info_valid) {
    ESP_LOGW(TAG, "Get IP info failed: network info not valid");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(info, &ctx->network_info, sizeof(lte_network_info_t));

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Network info returned");
  return ESP_OK;
}

/**
 * @brief Get IMEI string from context info
 */
esp_err_t lte_handler_get_imei(char *imei, size_t max_len) {
  if (!ctx || !imei || max_len == 0) {
    ESP_LOGE(TAG, "Get IMEI failed: bad args");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    ESP_LOGE(TAG, "Get IMEI failed: modem info not valid");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(imei, ctx->modem_info.imei, max_len - 1);
  imei[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Get IMEI: %s", imei);
  return ESP_OK;
}

/**
 * @brief Get IMSI string from context info
 */
esp_err_t lte_handler_get_imsi(char *imsi, size_t max_len) {
  if (!ctx || !imsi || max_len == 0) {
    ESP_LOGE(TAG, "Get IMSI failed: bad args");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    ESP_LOGE(TAG, "Get IMSI failed: modem info not valid");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(imsi, ctx->modem_info.imsi, max_len - 1);
  imsi[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Get IMSI: %s", imsi);
  return ESP_OK;
}

/**
 * @brief Copy modem info struct to user
 */
esp_err_t lte_handler_get_modem_info(lte_modem_info_t *info) {
  if (!ctx || !info) {
    ESP_LOGE(TAG, "Get modem info failed: bad args");
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    ESP_LOGE(TAG, "Get modem info failed: modem info not valid");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(info, &ctx->modem_info, sizeof(lte_modem_info_t));

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Modem info returned");
  return ESP_OK;
}

/**
 * @brief Set auto-reconnect logic for LTE, updates context flag
 */
esp_err_t lte_handler_set_auto_reconnect(bool enable) {
  if (!ctx) {
    ESP_LOGE(TAG, "Set auto-reconnect failed: no context");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  ctx->config.auto_reconnect = enable;

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Auto-reconnect set to %d", enable);
  return ESP_OK;
}

/**
 * @brief Set reconnect timing and attempt limits
 */
esp_err_t lte_handler_set_reconnect_params(uint32_t timeout_ms,
                                           uint32_t max_attempts) {
  if (!ctx) {
    ESP_LOGE(TAG, "Set reconnect params failed: no context");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  ctx->config.reconnect_timeout_ms = timeout_ms;
  ctx->config.max_reconnect_attempts = max_attempts;

  xSemaphoreGive(ctx->mutex);

  ESP_LOGI(TAG, "Reconnect params set: timeout=%lu, max_attempts=%lu",
           timeout_ms, max_attempts);
  return ESP_OK;
}
