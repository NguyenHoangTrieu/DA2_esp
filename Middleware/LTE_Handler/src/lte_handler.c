/**
 * @file lte_handler.c
 * @brief LTE Handler with UART and USB support
 */

#include "lte_handler.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lte_config.h"
#include <stdlib.h>
#include <string.h>

/* Include UART modem support */
#include "esp_modem_uart.h"
#include "esp_modem_uart_netif.h"

/* Include USB modem support */
#include "usbh_modem_board.h"

/* Include specific modem drivers for UART based on configuration */
#if LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_SIM7600
#include "sim7600_comm.h"
#define MODEM_UART_INIT_FUNC sim7600_init
#elif LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_BG96
#include "bg96_comm.h"
#define MODEM_UART_INIT_FUNC bg96_init
#else
#error "Unsupported modem device"
#endif

#define SIGNAL_POLL_INTERVAL_MS 10000
#define CONNECT_BIT BIT0
#define DISCONNECT_BIT BIT1

static const char *TAG = "LTE_HANDLER";
volatile bool g_not_ppp_to_lan = false;

ESP_EVENT_DEFINE_BASE(LTE_HANDLER_EVENT);

/**
 * @brief Internal context structure
 */
typedef struct {
  lte_handler_config_t config;
  lte_handler_state_t state;

  /* UART-specific members */
  modem_dte_t *uart_dte;
  modem_dce_t *uart_dce;
  void *uart_netif_adapter;

  /* USB-specific: modem board handles DTE/DCE internally */

  /* Common members */
  esp_netif_t *esp_netif;
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
 * @brief Set state with logging
 */
static void set_state(lte_handler_state_t new_state) {
  if (ctx) {
    if (ctx->mutex) {
      xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "State: %d -> %d", ctx->state, new_state);
    ctx->state = new_state;
    if (ctx->mutex) {
      xSemaphoreGive(ctx->mutex);
    }
  }
}

/**
 * @brief UART Modem event handler
 */
static void uart_modem_event_handler(void *event_handler_arg,
                                     esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  switch (event_id) {
  case ESP_MODEM_UART_EVENT_PPP_START:
    ESP_LOGI(TAG, "UART Modem PPP Started");
    break;
  case ESP_MODEM_UART_EVENT_PPP_STOP:
    ESP_LOGI(TAG, "UART Modem PPP Stopped");
    if (ctx && ctx->event_group) {
      xEventGroupSetBits(ctx->event_group, DISCONNECT_BIT);
    }
    set_state(LTE_STATE_DISCONNECTED);
    break;
  case ESP_MODEM_UART_EVENT_UNKNOWN:
    ESP_LOGW(TAG, "Unknown UART line: %s", (char *)event_data);
    break;
  default:
    break;
  }
}

/**
 * @brief USB Modem event handler (from modem_board)
 */
static void usb_modem_event(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
  if (event_base == MODEM_BOARD_EVENT) {
    switch (event_id) {
    case MODEM_EVENT_SIMCARD_DISCONN:
      ESP_LOGW(TAG, "Modem Event: SIM Card disconnected");
      break;
    case MODEM_EVENT_SIMCARD_CONN:
      ESP_LOGI(TAG, "Modem Event: SIM Card Connected");
      break;
    case MODEM_EVENT_DTE_DISCONN:
      ESP_LOGW(TAG, "Modem Event: USB disconnected");
      break;
    case MODEM_EVENT_DTE_CONN:
      ESP_LOGI(TAG, "Modem Event: USB connected");
      break;
    case MODEM_EVENT_DTE_RESTART:
      ESP_LOGW(TAG, "Modem Event: Hardware restart");
      break;
    case MODEM_EVENT_DTE_RESTART_DONE:
      ESP_LOGI(TAG, "Modem Event: Hardware restart done");
      break;
    case MODEM_EVENT_NET_CONN:
      ESP_LOGI(TAG, "Modem Event: Network connected");
      break;
    case MODEM_EVENT_NET_DISCONN:
      ESP_LOGW(TAG, "Modem Event: Network disconnected");
      break;
    case MODEM_EVENT_WIFI_STA_CONN:
      ESP_LOGI(TAG, "Modem Event: Station connected");
      break;
    case MODEM_EVENT_WIFI_STA_DISCONN:
      ESP_LOGW(TAG, "Modem Event: All stations disconnected");
      break;
    default:
      break;
    }
  }
}

/**
 * @brief IP event handler
 */
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
  ESP_LOGD(TAG, "IP event! %d", event_id);

  if (event_id == IP_EVENT_PPP_GOT_IP && !g_not_ppp_to_lan) {
    esp_netif_dns_info_t dns_info;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    esp_netif_t *netif = event->esp_netif;

    ESP_LOGI(TAG, "Modem PPP Connected");
    ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Netmask : " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway : " IPSTR, IP2STR(&event->ip_info.gw));

    if (ctx) {
      snprintf(ctx->network_info.ip, sizeof(ctx->network_info.ip), IPSTR,
               IP2STR(&event->ip_info.ip));
      snprintf(ctx->network_info.netmask, sizeof(ctx->network_info.netmask),
               IPSTR, IP2STR(&event->ip_info.netmask));
      snprintf(ctx->network_info.gateway, sizeof(ctx->network_info.gateway),
               IPSTR, IP2STR(&event->ip_info.gw));

      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
      snprintf(ctx->network_info.dns1, sizeof(ctx->network_info.dns1), IPSTR,
               IP2STR(&dns_info.ip.u_addr.ip4));

      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
      snprintf(ctx->network_info.dns2, sizeof(ctx->network_info.dns2), IPSTR,
               IP2STR(&dns_info.ip.u_addr.ip4));

      ctx->network_info_valid = true;
      set_state(LTE_STATE_CONNECTED);

      if (ctx->event_group) {
        xEventGroupSetBits(ctx->event_group, CONNECT_BIT);
      }
    }
  } else if (event_id == IP_EVENT_PPP_LOST_IP && !g_not_ppp_to_lan) {
    ESP_LOGI(TAG, "PPP Lost IP");
    if (ctx) {
      ctx->network_info_valid = false;
      set_state(LTE_STATE_DISCONNECTED);
    }
  }
}

/**
 * @brief PPP status handler
 */
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
  ESP_LOGI(TAG, "PPP state changed: %d", event_id);
}

/**
 * @brief Initialize UART modem
 */
static esp_err_t uart_modem_init(void) {
  ESP_LOGI(TAG, "Initializing UART modem...");

  /* Configure and create UART DTE */
  esp_modem_uart_dte_config_t dte_cfg = ESP_MODEM_UART_DTE_DEFAULT_CONFIG();
  ctx->uart_dte = esp_modem_uart_dte_init(&dte_cfg);
  if (!ctx->uart_dte) {
    ESP_LOGE(TAG, "Failed to init UART DTE");
    return ESP_FAIL;
  }

  /* Register UART modem event handler */
  ESP_ERROR_CHECK(esp_modem_uart_set_event_handler(uart_modem_event_handler,
                                                   ESP_EVENT_ANY_ID, NULL));

  /* Initialize DCE with specific modem driver */
  ESP_LOGI(TAG, "Initializing UART DCE...");
  ctx->uart_dce = MODEM_UART_INIT_FUNC(ctx->uart_dte);
  if (!ctx->uart_dce) {
    ESP_LOGE(TAG, "Failed to init DCE");
    return ESP_FAIL;
  }

  /* Configure modem */
  ESP_LOGI(TAG, "Configuring UART modem...");
  ESP_ERROR_CHECK(
      ctx->uart_dce->set_flow_ctrl(ctx->uart_dce, MODEM_FLOW_CONTROL_NONE));
  ESP_ERROR_CHECK(ctx->uart_dce->store_profile(ctx->uart_dce));

  /* Get modem information */
  strncpy(ctx->modem_info.module_name, ctx->uart_dce->name,
          sizeof(ctx->modem_info.module_name) - 1);
  strncpy(ctx->modem_info.operator_name, ctx->uart_dce->oper,
          sizeof(ctx->modem_info.operator_name) - 1);
  strncpy(ctx->modem_info.imei, ctx->uart_dce->imei,
          sizeof(ctx->modem_info.imei) - 1);
  strncpy(ctx->modem_info.imsi, ctx->uart_dce->imsi,
          sizeof(ctx->modem_info.imsi) - 1);

  /* Get signal quality */
  uint32_t rssi = 0, ber = 0;
  if (ctx->uart_dce->get_signal_quality(ctx->uart_dce, &rssi, &ber) == ESP_OK) {
    ctx->modem_info.rssi = rssi;
    ctx->modem_info.ber = ber;
  }

  ctx->modem_info_valid = true;

  ESP_LOGI(TAG, "UART Modem ready: %s", ctx->uart_dce->name);
  ESP_LOGI(TAG, "IMEI: %s", ctx->uart_dce->imei);

  return ESP_OK;
}

/**
 * @brief Initialize USB modem (via modem_board)
 */
static esp_err_t usb_modem_init(void) {
  ESP_LOGI(TAG, "Initializing USB modem...");

  /* Configure modem board */
  modem_config_t modem_config = MODEM_DEFAULT_CONFIG();

#ifdef CONFIG_MODEM_BOARD_INIT_ENTER_PPP
  modem_config.flags |= MODEM_FLAGS_INIT_NOT_ENTER_PPP;
  modem_config.flags |= MODEM_FLAGS_INIT_NOT_BLOCK;
#endif
  modem_config.handler = usb_modem_event;

  esp_err_t ret = modem_board_init(&modem_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Modem init failed: 0x%x", ret);
    return ret;
  }
  ESP_LOGI(TAG, "Modem initialized successfully");
  return ESP_OK;
}

/**
 * @brief Background task for signal polling and auto-reconnect
 */
static void lte_handler_bg_task(void *param) {
  ESP_LOGI(TAG, "Background task started");

  while (ctx && ctx->initialized) {
    /* Poll signal quality when connected */
    if (ctx->state == LTE_STATE_CONNECTED) {
      // if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      //   uint32_t rssi, ber;

      //   if (ctx->config.comm_type == LTE_HANDLER_UART && ctx->uart_dce) {
      //     if (ctx->uart_dce->get_signal_quality(ctx->uart_dce, &rssi, &ber) ==
      //         ESP_OK) {
      //       ctx->modem_info.rssi = rssi;
      //       ctx->modem_info.ber = ber;
      //     }
      //   } else if (ctx->config.comm_type == LTE_HANDLER_USB) {
      //     int rssi_int, ber_int;
      //     if (modem_board_get_signal_quality(&rssi_int, &ber_int) == ESP_OK) {
      //       ctx->modem_info.rssi = rssi_int;
      //       ctx->modem_info.ber = ber_int;
      //     }
      //   }
      //   xSemaphoreGive(ctx->mutex);
      // }
    }

    /* Auto-reconnect logic */
    if (ctx->config.auto_reconnect && ctx->state == LTE_STATE_DISCONNECTED &&
        (ctx->config.max_reconnect_attempts == 0 ||
         ctx->reconnect_attempts < ctx->config.max_reconnect_attempts)) {

      ctx->reconnect_attempts++;
      ESP_LOGW(TAG, "Auto reconnect attempt %lu", ctx->reconnect_attempts);
      set_state(LTE_STATE_RECONNECTING);
      vTaskDelay(pdMS_TO_TICKS(ctx->config.reconnect_timeout_ms));
      lte_handler_connect();
    }

    vTaskDelay(pdMS_TO_TICKS(SIGNAL_POLL_INTERVAL_MS));
  }

  ESP_LOGI(TAG, "Background task stopped");
  vTaskDelete(NULL);
}

/**
 * @brief Initialize LTE handler
 */
esp_err_t lte_handler_init(const lte_handler_config_t *config) {
  if (!config || !config->apn) {
    ESP_LOGE(TAG, "Invalid config");
    return ESP_ERR_INVALID_ARG;
  }

  if (ctx) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  ctx = calloc(1, sizeof(lte_handler_ctx_t));
  if (!ctx) {
    ESP_LOGE(TAG, "Failed to allocate context");
    return ESP_ERR_NO_MEM;
  }

  memcpy(&ctx->config, config, sizeof(lte_handler_config_t));
  ctx->mutex = xSemaphoreCreateMutex();
  ctx->event_group = xEventGroupCreate();

  if (!ctx->mutex || !ctx->event_group) {
    ESP_LOGE(TAG, "Failed to create sync objects");
    lte_handler_deinit();
    return ESP_ERR_NO_MEM;
  }

  set_state(LTE_STATE_INITIALIZING);
  ctx->initialized = true;

  /* Register event handlers */
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                             &on_ip_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                             &on_ppp_changed, NULL));

  /* Initialize modem based on communication type */
  esp_err_t ret;
  if (ctx->config.comm_type == LTE_HANDLER_UART) {
    ret = uart_modem_init();
    if (ret != ESP_OK) {
      lte_handler_deinit();
      return ret;
    }

    /* Create netif adapter for UART */
    ctx->uart_netif_adapter = esp_modem_uart_netif_setup(ctx->uart_dte);
    if (!ctx->uart_netif_adapter) {
      ESP_LOGE(TAG, "Failed to setup UART netif adapter");
      lte_handler_deinit();
      return ESP_FAIL;
    }

    /* Create PPP netif */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    ctx->esp_netif = esp_netif_new(&cfg);

    /* Set UART default handlers */
    esp_modem_uart_netif_set_default_handlers(ctx->esp_netif);

  } else if (ctx->config.comm_type == LTE_HANDLER_USB) {
    /* Set APN before initialization */
    ret = usb_modem_init();
    if (ret != ESP_OK) {
      lte_handler_deinit();
      return ret;
    }

  } else {
    ESP_LOGE(TAG, "Unknown comm type: %d", ctx->config.comm_type);
    lte_handler_deinit();
    return ESP_ERR_INVALID_ARG;
  }

  /* Set authentication if provided */
#if defined(CONFIG_LWIP_PPP_PAP_SUPPORT) ||                                    \
    defined(CONFIG_LWIP_PPP_CHAP_SUPPORT)
  if (ctx->esp_netif && ctx->config.username && ctx->config.password) {
#ifdef CONFIG_LWIP_PPP_PAP_SUPPORT
    esp_netif_ppp_set_auth(ctx->esp_netif, NETIF_PPP_AUTHTYPE_PAP,
                           ctx->config.username, ctx->config.password);
#else
    esp_netif_ppp_set_auth(ctx->esp_netif, NETIF_PPP_AUTHTYPE_CHAP,
                           ctx->config.username, ctx->config.password);
#endif
  }
#endif

  set_state(LTE_STATE_INITIALIZED);

  /* Create background task */
  xTaskCreate(lte_handler_bg_task, "lte_bg", 4096, NULL, 5, &ctx->bg_task);

  ESP_LOGI(TAG, "LTE handler initialized (%s mode)",
           ctx->config.comm_type == LTE_HANDLER_UART ? "UART" : "USB");
  set_state(LTE_STATE_CONNECTED);
  return ESP_OK;
}

/**
 * @brief Deinitialize LTE handler
 */
esp_err_t lte_handler_deinit(void) {
  ESP_LOGI(TAG, "Deinitializing...");

  if (!ctx) {
    return ESP_ERR_INVALID_STATE;
  }

  ctx->initialized = false;
  vTaskDelay(pdMS_TO_TICKS(100));

  /* Cleanup based on communication type */
  if (ctx->config.comm_type == LTE_HANDLER_UART) {
    if (ctx->uart_netif_adapter) {
      esp_modem_uart_netif_clear_default_handlers();
      esp_modem_uart_netif_teardown(ctx->uart_netif_adapter);
      ctx->uart_netif_adapter = NULL;
    }

    if (ctx->uart_dce) {
      ctx->uart_dce->deinit(ctx->uart_dce);
      ctx->uart_dce = NULL;
    }

    if (ctx->uart_dte) {
      ctx->uart_dte->deinit(ctx->uart_dte);
      ctx->uart_dte = NULL;
    }

    esp_modem_uart_remove_event_handler(uart_modem_event_handler);

  } else if (ctx->config.comm_type == LTE_HANDLER_USB) {
    modem_board_deinit();
  }

  if (ctx->esp_netif) {
    esp_netif_destroy(ctx->esp_netif);
    ctx->esp_netif = NULL;
  }

  /* Unregister event handlers */
  esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event);
  esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                               &on_ppp_changed);

  if (ctx->event_group) {
    vEventGroupDelete(ctx->event_group);
  }
  if (ctx->mutex) {
    vSemaphoreDelete(ctx->mutex);
  }

  free(ctx);
  ctx = NULL;

  ESP_LOGI(TAG, "Deinitialized");
  return ESP_OK;
}

/**
 * @brief Connect to network
 */
esp_err_t lte_handler_connect(void) {
  ESP_LOGI(TAG, "Connecting...");

  if (!ctx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (ctx->state == LTE_STATE_CONNECTED || ctx->state == LTE_STATE_CONNECTING) {
    ESP_LOGW(TAG, "Already connecting/connected");
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
  }

  set_state(LTE_STATE_CONNECTING);
  xSemaphoreGive(ctx->mutex);

  esp_err_t ret;

  if (ctx->config.comm_type == LTE_HANDLER_UART) {
    /* Define PDP context */
    ESP_ERROR_CHECK(ctx->uart_dce->define_pdp_context(ctx->uart_dce, 1, "IP",
                                                      ctx->config.apn));

    /* Attach netif */
    esp_netif_attach(ctx->esp_netif, ctx->uart_netif_adapter);

    /* Start PPP */
    ret = esp_modem_uart_start_ppp(ctx->uart_dte);

  } else { /* USB */
    /* Set APN using modem_board API */
    ret = modem_board_set_apn(ctx->config.apn, true);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set APN: %d", ret);
      set_state(LTE_STATE_ERROR);
      return ret;
    }

    /* Start PPP using modem_board API */
    ret = modem_board_ppp_start(60000);
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start PPP: %d", ret);
    set_state(LTE_STATE_ERROR);
    return ret;
  }

  /* Wait for connection */
  EventBits_t bits = xEventGroupWaitBits(ctx->event_group, CONNECT_BIT, pdTRUE,
                                         pdTRUE, pdMS_TO_TICKS(60000));
  if (bits & CONNECT_BIT) {
    ESP_LOGI(TAG, "Connected successfully");
    ctx->reconnect_attempts = 0;
    return ESP_OK;
  } else {
    ESP_LOGE(TAG, "Connection timeout");
    set_state(LTE_STATE_ERROR);
    return ESP_ERR_TIMEOUT;
  }
}

/**
 * @brief Disconnect from network
 */
esp_err_t lte_handler_disconnect(void) {
  ESP_LOGI(TAG, "Disconnecting...");

  if (!ctx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (ctx->state != LTE_STATE_CONNECTED) {
    ESP_LOGW(TAG, "Not connected");
    return ESP_OK;
  }

  esp_err_t ret;
  if (ctx->config.comm_type == LTE_HANDLER_UART) {
    ret = esp_modem_uart_stop_ppp(ctx->uart_dte);
  } else {
    ret = modem_board_ppp_stop(10000);
  }

  xEventGroupWaitBits(ctx->event_group, DISCONNECT_BIT, pdTRUE, pdTRUE,
                      pdMS_TO_TICKS(10000));

  set_state(LTE_STATE_DISCONNECTED);
  ctx->network_info_valid = false;

  ESP_LOGI(TAG, "Disconnected");
  return ret;
}

/**
 * @brief Get current state
 */
lte_handler_state_t lte_handler_get_state(void) {
  if (!ctx) {
    return LTE_STATE_IDLE;
  }

  lte_handler_state_t state;
  if (ctx->mutex) {
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    state = ctx->state;
    xSemaphoreGive(ctx->mutex);
  } else {
    state = ctx->state;
  }

  return state;
}

/**
 * @brief Check if connected
 */
bool lte_handler_is_connected(void) {
  return (ctx && ctx->state == LTE_STATE_CONNECTED);
}

/**
 * @brief Get signal strength
 */
esp_err_t lte_handler_get_signal_strength(uint32_t *rssi, uint32_t *ber) {
  if (!ctx || !rssi || !ber) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t ret;
  if (ctx->config.comm_type == LTE_HANDLER_UART && ctx->uart_dce) {
    ret = ctx->uart_dce->get_signal_quality(ctx->uart_dce, rssi, ber);
  } else if (ctx->config.comm_type == LTE_HANDLER_USB) {
    int rssi_int, ber_int;
    ret = modem_board_get_signal_quality(&rssi_int, &ber_int);
    *rssi = rssi_int;
    *ber = ber_int;
  } else {
    ret = ESP_ERR_INVALID_STATE;
  }

  if (ret == ESP_OK) {
    ctx->modem_info.rssi = *rssi;
    ctx->modem_info.ber = *ber;
  }

  xSemaphoreGive(ctx->mutex);
  return ret;
}

/**
 * @brief Get operator name
 */
esp_err_t lte_handler_get_operator_name(char *operator_name, size_t max_len) {
  if (!ctx || !operator_name || max_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(operator_name, ctx->modem_info.operator_name, max_len - 1);
  operator_name[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);
  return ESP_OK;
}

/**
 * @brief Get IP info
 */
esp_err_t lte_handler_get_ip_info(lte_network_info_t *info) {
  if (!ctx || !info) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->network_info_valid) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(info, &ctx->network_info, sizeof(lte_network_info_t));

  xSemaphoreGive(ctx->mutex);
  return ESP_OK;
}

/**
 * @brief Get IMEI
 */
esp_err_t lte_handler_get_imei(char *imei, size_t max_len) {
  if (!ctx || !imei || max_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(imei, ctx->modem_info.imei, max_len - 1);
  imei[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);
  return ESP_OK;
}

/**
 * @brief Get IMSI
 */
esp_err_t lte_handler_get_imsi(char *imsi, size_t max_len) {
  if (!ctx || !imsi || max_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  strncpy(imsi, ctx->modem_info.imsi, max_len - 1);
  imsi[max_len - 1] = '\0';

  xSemaphoreGive(ctx->mutex);
  return ESP_OK;
}

/**
 * @brief Get modem info
 */
esp_err_t lte_handler_get_modem_info(lte_modem_info_t *info) {
  if (!ctx || !info) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!ctx->modem_info_valid) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(info, &ctx->modem_info, sizeof(lte_modem_info_t));

  xSemaphoreGive(ctx->mutex);
  return ESP_OK;
}

/**
 * @brief Set auto-reconnect
 */
esp_err_t lte_handler_set_auto_reconnect(bool enable) {
  if (!ctx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  ctx->config.auto_reconnect = enable;

  xSemaphoreGive(ctx->mutex);
  ESP_LOGI(TAG, "Auto-reconnect: %d", enable);
  return ESP_OK;
}

/**
 * @brief Set reconnect parameters
 */
esp_err_t lte_handler_set_reconnect_params(uint32_t timeout_ms,
                                           uint32_t max_attempts) {
  if (!ctx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  ctx->config.reconnect_timeout_ms = timeout_ms;
  ctx->config.max_reconnect_attempts = max_attempts;

  xSemaphoreGive(ctx->mutex);
  ESP_LOGI(TAG, "Reconnect params: timeout=%lu ms, max=%lu", timeout_ms,
           max_attempts);
  return ESP_OK;
}
