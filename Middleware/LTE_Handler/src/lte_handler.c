/**
 * LTE Handler Implementation
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"

#include "lte_handler.h"
#include "lte_config.h"

/* BSP Layer includes - abstraction */
#include "esp_modem.h"
#include "esp_modem_netif.h"

/* Include specific modem drivers based on configuration */
#if LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_SIM7600
#include "sim7600_comm.h"
#define MODEM_INIT_FUNC sim7600_init
#elif LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_BG96
#include "bg96_comm.h"
#define MODEM_INIT_FUNC bg96_init
#elif LTE_CONFIG_MODEM_DEVICE == LTE_CONFIG_MODEM_DEVICE_SIM800
#include "sim800.h"
#define MODEM_INIT_FUNC sim800_init
#else
#error "Unsupported modem device"
#endif

static const char *TAG = "lte_handler";

/* Event base definition */
ESP_EVENT_DEFINE_BASE(LTE_HANDLER_EVENT);

/* Event group bits */
#define LTE_CONNECTED_BIT       BIT0
#define LTE_DISCONNECT_BIT      BIT1
#define LTE_STOP_BIT            BIT2
#define LTE_RECONNECT_BIT       BIT3

/**
 * @brief LTE Handler Context (Private)
 */
typedef struct {
    /* Configuration */
    lte_handler_config_t config;
    
    /* State management */
    lte_handler_state_t state;
    SemaphoreHandle_t state_mutex;
    EventGroupHandle_t event_group;
    
    /* BSP Layer objects */
    modem_dte_t *dte;
    modem_dce_t *dce;
    void *modem_netif_adapter;
    
    /* Network interface */
    esp_netif_t *esp_netif;
    
    /* Modem information cache */
    lte_modem_info_t modem_info;
    lte_network_info_t network_info;
    bool modem_info_valid;
    bool network_info_valid;
    
    /* Auto-reconnect management */
    uint32_t reconnect_attempts;
    TaskHandle_t monitor_task_handle;
    bool initialized;
    
} lte_handler_context_t;

/* Global context (singleton pattern) */
static lte_handler_context_t *s_lte_ctx = NULL;

/* Forward declarations */
static void lte_state_machine_task(void *pvParameters);
static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, 
                                int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void ppp_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static esp_err_t lte_init_modem(void);
static esp_err_t lte_start_ppp_connection(void);
static esp_err_t lte_stop_ppp_connection(void);
static void lte_set_state(lte_handler_state_t new_state);
static esp_err_t lte_post_event(lte_handler_event_t event, void *data, size_t data_size);

/**
 * @brief Set handler state (thread-safe)
 */
static void lte_set_state(lte_handler_state_t new_state)
{
    if (s_lte_ctx && s_lte_ctx->state_mutex) {
        xSemaphoreTake(s_lte_ctx->state_mutex, portMAX_DELAY);
        s_lte_ctx->state = new_state;
        xSemaphoreGive(s_lte_ctx->state_mutex);
        
        ESP_LOGI(TAG, "State changed to: %d", new_state);
    }
}

/**
 * @brief Post event to system event loop
 */
static esp_err_t lte_post_event(lte_handler_event_t event, void *data, size_t data_size)
{
    return esp_event_post(LTE_HANDLER_EVENT, event, data, data_size, portMAX_DELAY);
}

/**
 * @brief Modem event handler (BSP layer events)
 */
static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, 
                                int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ESP_MODEM_EVENT_PPP_START:
            ESP_LOGI(TAG, "Modem PPP Started");
            lte_set_state(LTE_STATE_CONNECTING);
            lte_post_event(LTE_EVENT_CONNECTING, NULL, 0);
            break;
            
        case ESP_MODEM_EVENT_PPP_STOP:
            ESP_LOGI(TAG, "Modem PPP Stopped");
            xEventGroupSetBits(s_lte_ctx->event_group, LTE_STOP_BIT);
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
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;
        esp_netif_dns_info_t dns_info;
        
        /* Cache network information */
        if (s_lte_ctx) {
            snprintf(s_lte_ctx->network_info.ip, sizeof(s_lte_ctx->network_info.ip),
                    IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_lte_ctx->network_info.netmask, sizeof(s_lte_ctx->network_info.netmask),
                    IPSTR, IP2STR(&event->ip_info.netmask));
            snprintf(s_lte_ctx->network_info.gateway, sizeof(s_lte_ctx->network_info.gateway),
                    IPSTR, IP2STR(&event->ip_info.gw));
            
            esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
            snprintf(s_lte_ctx->network_info.dns1, sizeof(s_lte_ctx->network_info.dns1),
                    IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            
            esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
            snprintf(s_lte_ctx->network_info.dns2, sizeof(s_lte_ctx->network_info.dns2),
                    IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            
            s_lte_ctx->network_info_valid = true;
            s_lte_ctx->reconnect_attempts = 0;  // Reset reconnect counter
        }
        
        ESP_LOGI(TAG, "==== LTE Connected ====");
        ESP_LOGI(TAG, "IP          : %s", s_lte_ctx->network_info.ip);
        ESP_LOGI(TAG, "Netmask     : %s", s_lte_ctx->network_info.netmask);
        ESP_LOGI(TAG, "Gateway     : %s", s_lte_ctx->network_info.gateway);
        ESP_LOGI(TAG, "DNS1        : %s", s_lte_ctx->network_info.dns1);
        ESP_LOGI(TAG, "DNS2        : %s", s_lte_ctx->network_info.dns2);
        ESP_LOGI(TAG, "=======================");
        
        lte_set_state(LTE_STATE_CONNECTED);
        xEventGroupSetBits(s_lte_ctx->event_group, LTE_CONNECTED_BIT);
        lte_post_event(LTE_EVENT_CONNECTED, NULL, 0);
        
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Disconnected from PPP Server");
        
        if (s_lte_ctx) {
            s_lte_ctx->network_info_valid = false;
            lte_set_state(LTE_STATE_DISCONNECTED);
            xEventGroupSetBits(s_lte_ctx->event_group, LTE_DISCONNECT_BIT);
            lte_post_event(LTE_EVENT_DISCONNECTED, NULL, 0);
            
            /* Trigger auto-reconnect if enabled */
            if (s_lte_ctx->config.auto_reconnect) {
                xEventGroupSetBits(s_lte_ctx->event_group, LTE_RECONNECT_BIT);
            }
        }
    }
}

/**
 * @brief PPP status event handler
 */
static void ppp_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "PPP state changed event: %ld", event_id);
    
    if (event_id == NETIF_PPP_ERRORUSER) {
        esp_netif_t *netif = (esp_netif_t *)event_data;
        ESP_LOGI(TAG, "User interrupted event from netif: %p", netif);
    }
}

/**
 * @brief Initialize modem (BSP layer)
 */
static esp_err_t lte_init_modem(void)
{
    esp_err_t ret;
    
    /* Create DTE configuration */
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    // dte_config.tx_io_num = LTE_CONFIG_UART_TX_PIN;
    // dte_config.rx_io_num = LTE_CONFIG_UART_RX_PIN;
    // dte_config.rts_io_num = LTE_CONFIG_UART_RTS_PIN;
    // dte_config.cts_io_num = LTE_CONFIG_UART_CTS_PIN;
    // dte_config.rx_buffer_size = LTE_CONFIG_UART_RX_BUFFER_SIZE;
    // dte_config.tx_buffer_size = LTE_CONFIG_UART_TX_BUFFER_SIZE;
    // dte_config.pattern_queue_size = LTE_CONFIG_UART_PATTERN_QUEUE_SIZE;
    // dte_config.event_queue_size = LTE_CONFIG_UART_EVENT_QUEUE_SIZE;
    // dte_config.event_task_stack_size = LTE_CONFIG_UART_EVENT_TASK_STACK_SIZE;
    // dte_config.event_task_priority = LTE_CONFIG_UART_EVENT_TASK_PRIORITY;
    // dte_config.line_buffer_size = LTE_CONFIG_UART_RX_BUFFER_SIZE * 2;
    
    /* Initialize DTE */
    s_lte_ctx->dte = esp_modem_dte_init(&dte_config);
    if (s_lte_ctx->dte == NULL) {
        ESP_LOGE(TAG, "Failed to initialize DTE");
        return ESP_FAIL;
    }
    
    /* Register modem event handler */
    ret = esp_modem_set_event_handler(s_lte_ctx->dte, modem_event_handler, 
                                      ESP_EVENT_ANY_ID, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set modem event handler");
        return ret;
    }
    
    /* Initialize DCE (Device Communication Equipment) */
    uint8_t retry = 0;
    do {
        ESP_LOGI(TAG, "Initializing modem (attempt %d)...", retry + 1);
        s_lte_ctx->dce = MODEM_INIT_FUNC(s_lte_ctx->dte);
        if (s_lte_ctx->dce == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            retry++;
        }
    } while (s_lte_ctx->dce == NULL && retry < 5);
    
    if (s_lte_ctx->dce == NULL) {
        ESP_LOGE(TAG, "Failed to initialize DCE after %d attempts", retry);
        return ESP_FAIL;
    }
    
    /* Enable CMUX (allows simultaneous data and AT commands) */
    ret = esp_modem_start_cmux(s_lte_ctx->dte);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start CMUX, continuing without it");
    }
    
    /* Configure modem */
    s_lte_ctx->dce->set_flow_ctrl(s_lte_ctx->dce, MODEM_FLOW_CONTROL_NONE);
    s_lte_ctx->dce->store_profile(s_lte_ctx->dce);
    
    /* Cache modem information */
    strncpy(s_lte_ctx->modem_info.module_name, s_lte_ctx->dce->name, 
            sizeof(s_lte_ctx->modem_info.module_name) - 1);
    strncpy(s_lte_ctx->modem_info.operator_name, s_lte_ctx->dce->oper, 
            sizeof(s_lte_ctx->modem_info.operator_name) - 1);
    strncpy(s_lte_ctx->modem_info.imei, s_lte_ctx->dce->imei, 
            sizeof(s_lte_ctx->modem_info.imei) - 1);
    strncpy(s_lte_ctx->modem_info.imsi, s_lte_ctx->dce->imsi, 
            sizeof(s_lte_ctx->modem_info.imsi) - 1);
    
    /* Get initial signal quality */
    s_lte_ctx->dce->get_signal_quality(s_lte_ctx->dce, 
                                      &s_lte_ctx->modem_info.rssi, 
                                      &s_lte_ctx->modem_info.ber);
    
    s_lte_ctx->modem_info_valid = true;
    
    ESP_LOGI(TAG, "Modem Info:");
    ESP_LOGI(TAG, "  Module: %s", s_lte_ctx->modem_info.module_name);
    ESP_LOGI(TAG, "  Operator: %s", s_lte_ctx->modem_info.operator_name);
    ESP_LOGI(TAG, "  IMEI: %s", s_lte_ctx->modem_info.imei);
    ESP_LOGI(TAG, "  IMSI: %s", s_lte_ctx->modem_info.imsi);
    ESP_LOGI(TAG, "  RSSI: %lu, BER: %lu", s_lte_ctx->modem_info.rssi, s_lte_ctx->modem_info.ber);
    
    return ESP_OK;
}

/**
 * @brief Start PPP connection
 */
static esp_err_t lte_start_ppp_connection(void)
{
    esp_err_t ret;
    
    if (s_lte_ctx->dce == NULL || s_lte_ctx->esp_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Define PDP context with APN */
    ret = s_lte_ctx->dce->define_pdp_context(s_lte_ctx->dce, 1, "IP", s_lte_ctx->config.apn);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to define PDP context");
        return ret;
    }
    
    /* Set PPP authentication if configured */
#if !LTE_CONFIG_PPP_AUTH_NONE
    if (s_lte_ctx->config.username && s_lte_ctx->config.password) {
#if CONFIG_LWIP_PPP_PAP_SUPPORT
        esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_PAP;
#elif CONFIG_LWIP_PPP_CHAP_SUPPORT
        esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_CHAP;
#else
        ESP_LOGW(TAG, "PPP authentication not supported");
#endif
#if defined(CONFIG_LWIP_PPP_PAP_SUPPORT) || defined(CONFIG_LWIP_PPP_CHAP_SUPPORT)
        esp_netif_ppp_set_auth(s_lte_ctx->esp_netif, auth_type, 
                              s_lte_ctx->config.username, 
                              s_lte_ctx->config.password);
#endif
    }
#endif
    
    /* Attach modem to netif */
    esp_netif_attach(s_lte_ctx->esp_netif, s_lte_ctx->modem_netif_adapter);
    
    /* Start PPP */
    ret = esp_modem_start_ppp(s_lte_ctx->dte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PPP");
        return ret;
    }
    
    ESP_LOGI(TAG, "PPP started, waiting for IP address...");
    return ESP_OK;
}

/**
 * @brief Stop PPP connection
 */
static esp_err_t lte_stop_ppp_connection(void)
{
    if (s_lte_ctx->dte == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_modem_stop_ppp(s_lte_ctx->dte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop PPP");
        return ret;
    }
    
    /* Wait for stop confirmation */
    EventBits_t bits = xEventGroupWaitBits(s_lte_ctx->event_group, 
                                          LTE_STOP_BIT,
                                          pdTRUE, pdTRUE, 
                                          pdMS_TO_TICKS(10000));
    
    if (!(bits & LTE_STOP_BIT)) {
        ESP_LOGW(TAG, "PPP stop timeout");
    }
    
    return ESP_OK;
}

/**
 * @brief State machine monitoring task
 */
static void lte_state_machine_task(void *pvParameters)
{
    ESP_LOGI(TAG, "LTE state machine task started");
    
    while (s_lte_ctx && s_lte_ctx->initialized) {
        EventBits_t bits = xEventGroupWaitBits(s_lte_ctx->event_group,
                                              LTE_RECONNECT_BIT | LTE_DISCONNECT_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(5000));
        
        if (bits & LTE_RECONNECT_BIT) {
            /* Auto-reconnect triggered */
            if (s_lte_ctx->config.auto_reconnect && 
                s_lte_ctx->state == LTE_STATE_DISCONNECTED) {
                
                /* Check max reconnect attempts */
                if (s_lte_ctx->config.max_reconnect_attempts > 0 &&
                    s_lte_ctx->reconnect_attempts >= s_lte_ctx->config.max_reconnect_attempts) {
                    ESP_LOGE(TAG, "Max reconnect attempts reached");
                    lte_post_event(LTE_EVENT_ERROR, NULL, 0);
                    continue;
                }
                
                s_lte_ctx->reconnect_attempts++;
                ESP_LOGI(TAG, "Auto-reconnecting (attempt %lu)...", 
                        s_lte_ctx->reconnect_attempts);
                
                lte_set_state(LTE_STATE_RECONNECTING);
                lte_post_event(LTE_EVENT_RECONNECTING, NULL, 0);
                
                /* Wait before reconnecting */
                vTaskDelay(pdMS_TO_TICKS(s_lte_ctx->config.reconnect_timeout_ms));
                
                /* Attempt reconnection */
                if (lte_handler_connect() == ESP_OK) {
                    ESP_LOGI(TAG, "Reconnection initiated");
                } else {
                    ESP_LOGE(TAG, "Reconnection failed, will retry");
                    xEventGroupSetBits(s_lte_ctx->event_group, LTE_RECONNECT_BIT);
                }
            }
        }
        
        /* Periodic signal quality update if connected */
        if (s_lte_ctx->state == LTE_STATE_CONNECTED && s_lte_ctx->dce) {
            s_lte_ctx->dce->get_signal_quality(s_lte_ctx->dce,
                                              &s_lte_ctx->modem_info.rssi,
                                              &s_lte_ctx->modem_info.ber);
            ESP_LOGD(TAG, "Signal: RSSI=%lu, BER=%lu", 
                    s_lte_ctx->modem_info.rssi, s_lte_ctx->modem_info.ber);
        }
    }
    
    ESP_LOGI(TAG, "LTE state machine task stopped");
    vTaskDelete(NULL);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t lte_handler_init(const lte_handler_config_t *config)
{
    if (config == NULL || config->apn == NULL) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    /* Allocate context */
    s_lte_ctx = calloc(1, sizeof(lte_handler_context_t));
    if (s_lte_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }
    
    /* Copy configuration */
    memcpy(&s_lte_ctx->config, config, sizeof(lte_handler_config_t));
    
    /* Duplicate string pointers */
    s_lte_ctx->config.apn = strdup(config->apn);
    if (config->username) {
        s_lte_ctx->config.username = strdup(config->username);
    }
    if (config->password) {
        s_lte_ctx->config.password = strdup(config->password);
    }
    
    /* Create synchronization primitives */
    s_lte_ctx->state_mutex = xSemaphoreCreateMutex();
    s_lte_ctx->event_group = xEventGroupCreate();
    
    if (s_lte_ctx->state_mutex == NULL || s_lte_ctx->event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        lte_handler_deinit();
        return ESP_ERR_NO_MEM;
    }
    
    /* Initialize state */
    s_lte_ctx->state = LTE_STATE_IDLE;
    s_lte_ctx->initialized = true;
    s_lte_ctx->reconnect_attempts = 0;
    
    /* Register IP event handlers */
    esp_err_t ret = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, 
                                               &ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        lte_handler_deinit();
        return ret;
    }
    
    ret = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, 
                                     &ppp_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPP event handler");
        lte_handler_deinit();
        return ret;
    }
    
    /* Initialize network interface */
    esp_netif_inherent_config_t base_config = {
        .flags = ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED,
        .lost_ip_event = IP_EVENT_PPP_LOST_IP,
        .get_ip_event = IP_EVENT_PPP_GOT_IP,
        .if_key = "PPP_DEF",
        .if_desc = "ppp",
        .route_prio = 30
    };

    extern const esp_netif_netstack_config_t *_g_esp_netif_netstack_default_ppp;

    esp_netif_config_t netif_cfg = {
        .base = &base_config,
        .driver = NULL,
        .stack = _g_esp_netif_netstack_default_ppp,
    };

    s_lte_ctx->esp_netif = esp_netif_new(&netif_cfg);
    if (s_lte_ctx->esp_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create netif");
        lte_handler_deinit();
        return ESP_FAIL;
    }
    
    lte_set_state(LTE_STATE_INITIALIZING);
    
    /* Initialize modem (BSP layer) */
    ret = lte_init_modem();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize modem");
        lte_handler_deinit();
        return ret;
    }
    
    /* Setup netif adapter */
    s_lte_ctx->modem_netif_adapter = esp_modem_netif_setup(s_lte_ctx->dte);
    esp_modem_netif_set_default_handlers(s_lte_ctx->modem_netif_adapter, 
                                        s_lte_ctx->esp_netif);
    
    lte_set_state(LTE_STATE_INITIALIZED);
    lte_post_event(LTE_EVENT_INITIALIZED, NULL, 0);
    
    /* Create state machine task */
    BaseType_t task_ret = xTaskCreate(lte_state_machine_task, "lte_sm",
                                     4096, NULL, 5,
                                     &s_lte_ctx->monitor_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create state machine task");
        lte_handler_deinit();
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "LTE Handler initialized successfully");
    ESP_LOGI(TAG, "  APN: %s", s_lte_ctx->config.apn);
    ESP_LOGI(TAG, "  Auto-reconnect: %s", 
            s_lte_ctx->config.auto_reconnect ? "enabled" : "disabled");
    
    return ESP_OK;
}

esp_err_t lte_handler_deinit(void)
{
    if (s_lte_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Deinitializing LTE Handler...");
    
    /* Mark as not initialized to stop tasks */
    s_lte_ctx->initialized = false;
    
    /* Disconnect if connected */
    if (s_lte_ctx->state == LTE_STATE_CONNECTED) {
        lte_handler_disconnect();
    }
    
    /* Wait for monitor task to finish */
    if (s_lte_ctx->monitor_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    /* Cleanup netif */
    if (s_lte_ctx->modem_netif_adapter) {
        esp_modem_netif_clear_default_handlers(s_lte_ctx->modem_netif_adapter);
        esp_modem_netif_teardown(s_lte_ctx->modem_netif_adapter);
    }
    
    if (s_lte_ctx->esp_netif) {
        esp_netif_destroy(s_lte_ctx->esp_netif);
    }
    
    /* Cleanup modem */
    if (s_lte_ctx->dce) {
        s_lte_ctx->dce->deinit(s_lte_ctx->dce);
    }
    
    if (s_lte_ctx->dte) {
        s_lte_ctx->dte->deinit(s_lte_ctx->dte);
    }
    
    /* Unregister event handlers */
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler);
    esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &ppp_event_handler);
    
    /* Free sync primitives */
    if (s_lte_ctx->state_mutex) {
        vSemaphoreDelete(s_lte_ctx->state_mutex);
    }
    
    if (s_lte_ctx->event_group) {
        vEventGroupDelete(s_lte_ctx->event_group);
    }
    
    /* Free configuration strings */
    if (s_lte_ctx->config.apn) {
        free((void *)s_lte_ctx->config.apn);
    }
    if (s_lte_ctx->config.username) {
        free((void *)s_lte_ctx->config.username);
    }
    if (s_lte_ctx->config.password) {
        free((void *)s_lte_ctx->config.password);
    }
    
    /* Free context */
    free(s_lte_ctx);
    s_lte_ctx = NULL;
    
    ESP_LOGI(TAG, "LTE Handler deinitialized");
    return ESP_OK;
}

esp_err_t lte_handler_connect(void)
{
    if (s_lte_ctx == NULL || !s_lte_ctx->initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_lte_ctx->state == LTE_STATE_CONNECTED || 
        s_lte_ctx->state == LTE_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connected or connecting");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Connecting to LTE network...");
    ESP_LOGI(TAG, "APN: %s", s_lte_ctx->config.apn);
    
    lte_set_state(LTE_STATE_CONNECTING);
    
    esp_err_t ret = lte_start_ppp_connection();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PPP connection");
        lte_set_state(LTE_STATE_ERROR);
        lte_post_event(LTE_EVENT_ERROR, NULL, 0);
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t lte_handler_disconnect(void)
{
    if (s_lte_ctx == NULL || !s_lte_ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_lte_ctx->state != LTE_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Not connected");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Disconnecting from LTE network...");
    
    esp_err_t ret = lte_stop_ppp_connection();
    if (ret == ESP_OK) {
        lte_set_state(LTE_STATE_DISCONNECTED);
        s_lte_ctx->network_info_valid = false;
        lte_post_event(LTE_EVENT_DISCONNECTED, NULL, 0);
    }
    
    return ret;
}

lte_handler_state_t lte_handler_get_state(void)
{
    if (s_lte_ctx == NULL) {
        return LTE_STATE_IDLE;
    }
    
    lte_handler_state_t state;
    xSemaphoreTake(s_lte_ctx->state_mutex, portMAX_DELAY);
    state = s_lte_ctx->state;
    xSemaphoreGive(s_lte_ctx->state_mutex);
    
    return state;
}

bool lte_handler_is_connected(void)
{
    return (lte_handler_get_state() == LTE_STATE_CONNECTED);
}

esp_err_t lte_handler_get_signal_strength(uint32_t *rssi, uint32_t *ber)
{
    if (rssi == NULL || ber == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->modem_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Update from modem */
    if (s_lte_ctx->dce) {
        esp_err_t ret = s_lte_ctx->dce->get_signal_quality(s_lte_ctx->dce, rssi, ber);
        if (ret == ESP_OK) {
            s_lte_ctx->modem_info.rssi = *rssi;
            s_lte_ctx->modem_info.ber = *ber;
        }
        return ret;
    }
    
    /* Return cached values */
    *rssi = s_lte_ctx->modem_info.rssi;
    *ber = s_lte_ctx->modem_info.ber;
    
    return ESP_OK;
}

esp_err_t lte_handler_get_operator_name(char *operator_name, size_t max_len)
{
    if (operator_name == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->modem_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(operator_name, s_lte_ctx->modem_info.operator_name, max_len - 1);
    operator_name[max_len - 1] = '\0';
    
    return ESP_OK;
}

esp_err_t lte_handler_get_ip_info(lte_network_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->network_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memcpy(info, &s_lte_ctx->network_info, sizeof(lte_network_info_t));
    return ESP_OK;
}

esp_err_t lte_handler_get_imei(char *imei, size_t max_len)
{
    if (imei == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->modem_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(imei, s_lte_ctx->modem_info.imei, max_len - 1);
    imei[max_len - 1] = '\0';
    
    return ESP_OK;
}

esp_err_t lte_handler_get_imsi(char *imsi, size_t max_len)
{
    if (imsi == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->modem_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(imsi, s_lte_ctx->modem_info.imsi, max_len - 1);
    imsi[max_len - 1] = '\0';
    
    return ESP_OK;
}

esp_err_t lte_handler_get_modem_info(lte_modem_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_lte_ctx == NULL || !s_lte_ctx->modem_info_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Update signal quality before returning */
    if (s_lte_ctx->dce) {
        s_lte_ctx->dce->get_signal_quality(s_lte_ctx->dce,
                                          &s_lte_ctx->modem_info.rssi,
                                          &s_lte_ctx->modem_info.ber);
    }
    
    memcpy(info, &s_lte_ctx->modem_info, sizeof(lte_modem_info_t));
    return ESP_OK;
}

esp_netif_t *lte_handler_get_netif(void)
{
    if (s_lte_ctx == NULL) {
        return NULL;
    }
    
    return s_lte_ctx->esp_netif;
}

esp_err_t lte_handler_set_auto_reconnect(bool enable)
{
    if (s_lte_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_lte_ctx->config.auto_reconnect = enable;
    ESP_LOGI(TAG, "Auto-reconnect %s", enable ? "enabled" : "disabled");
    
    return ESP_OK;
}

esp_err_t lte_handler_set_reconnect_params(uint32_t timeout_ms, uint32_t max_attempts)
{
    if (s_lte_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_lte_ctx->config.reconnect_timeout_ms = timeout_ms;
    s_lte_ctx->config.max_reconnect_attempts = max_attempts;
    
    ESP_LOGI(TAG, "Reconnect params: timeout=%lums, max_attempts=%lu",
            timeout_ms, max_attempts);
    
    return ESP_OK;
}
