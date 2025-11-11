/*
 * PPP Server Component for WAN MCU (Using eppp_link)
 * 
 * This component uses official Espressif eppp_link to create a PPP server
 * over UART, sharing Wi-Fi connection with LAN MCU and managing OTA.
 */

#include "ppp_server.h"
#include "ppp_server_config.h"
#include "fota_handler.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/uart.h"

// eppp_link API
#include "eppp_link.h"

// For NAPT support
#if CONFIG_LWIP_IPV4_NAPT
#include "lwip/lwip_napt.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "ppp_server";

/* PPP Server State - eppp_listen returns esp_netif_t* */
static esp_netif_t *s_ppp_netif = NULL;
static bool s_ppp_server_initialized = false;
static bool s_ppp_client_connected = false;

/* Response tracking for OTA control */
static SemaphoreHandle_t s_response_semaphore = NULL;
static esp_err_t s_last_response_result = ESP_ERR_TIMEOUT;

/* Event group for PPP events */
static EventGroupHandle_t s_ppp_event_group = NULL;
#define PPP_CLIENT_CONNECTED_BIT BIT0

/* Function Prototypes */
static esp_err_t ppp_server_enable_napt(esp_netif_t *wifi_netif);
static void ppp_server_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);
static void ppp_server_task(void *pvParameters);

/**
 * @brief Enable NAPT on the Wi-Fi interface
 */
static esp_err_t ppp_server_enable_napt(esp_netif_t *wifi_netif)
{
    if (!wifi_netif) {
        ESP_LOGE(TAG, "Invalid wifi_netif");
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_LWIP_IPV4_NAPT
    // Get IP info from WiFi interface
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(wifi_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(err));
        return err;
    }

    // Check if IP is valid
    if (ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "WiFi interface has no valid IP address");
        return ESP_ERR_INVALID_STATE;
    }

    // Enable NAPT
    uint32_t napt_ip = ip_info.ip.addr;
    ip_napt_enable(napt_ip, 1);
    
    ESP_LOGI(TAG, "NAPT enabled on Wi-Fi interface (IP: " IPSTR ")", 
             IP2STR(&ip_info.ip));
    return ESP_OK;
#else
    ESP_LOGE(TAG, "NAPT is not enabled in menuconfig");
    ESP_LOGE(TAG, "Please enable CONFIG_LWIP_IPV4_NAPT");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief PPP Server event handler
 */
static void ppp_server_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_PPP_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "================================================================");
            ESP_LOGI(TAG, "PPP Client Connected!");
            ESP_LOGI(TAG, "Client IP : " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Netmask   : " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "Gateway   : " IPSTR, IP2STR(&event->ip_info.gw));
            ESP_LOGI(TAG, "================================================================");
            
            s_ppp_client_connected = true;
            xEventGroupSetBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
        }
        else if (event_id == IP_EVENT_PPP_LOST_IP) {
            ESP_LOGW(TAG, "PPP Client Disconnected");
            s_ppp_client_connected = false;
            xEventGroupClearBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
        }
    }
}

/**
 * @brief PPP server task that runs eppp_listen()
 * 
 * This task blocks on eppp_listen() until a client connects
 */
static void ppp_server_task(void *pvParameters)
{
    ESP_LOGI(TAG, "PPP server task started");

    // Configure eppp for UART transport
    // Note: eppp_config_t structure is simpler - just set transport type
    eppp_config_t config = {
        .transport = EPPP_TRANSPORT_UART,
        .uart.port = PPP_UART_PORT,
        .uart.tx_io = PPP_UART_TX_PIN,
        .uart.rx_io = PPP_UART_RX_PIN,
        .uart.baud = PPP_UART_BAUD_RATE,
        .uart.rx_buffer_size = PPP_UART_BUF_SIZE,
        .uart.queue_size = PPP_UART_QUEUE_SIZE,
    };

    // Use simplified blocking API: eppp_listen()
    // This will block until a client connects
    ESP_LOGI(TAG, "Waiting for PPP client connection...");
    
    s_ppp_netif = eppp_listen(&config);
    
    if (s_ppp_netif) {
        ESP_LOGI(TAG, "PPP client connected successfully");
        s_ppp_client_connected = true;
        xEventGroupSetBits(s_ppp_event_group, PPP_CLIENT_CONNECTED_BIT);
    } else {
        ESP_LOGE(TAG, "Failed to establish PPP connection");
    }

    // Task stays alive to keep connection
    while (s_ppp_server_initialized) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "PPP server task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Initialize the PPP server using eppp_link
 */
esp_err_t ppp_server_init(esp_netif_t *wifi_netif)
{
    if (s_ppp_server_initialized) {
        ESP_LOGW(TAG, "PPP server already initialized");
        return ESP_OK;
    }

    if (!wifi_netif) {
        ESP_LOGE(TAG, "Invalid wifi_netif parameter");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing PPP server using eppp_link...");

    // Create event group
    s_ppp_event_group = xEventGroupCreate();
    if (!s_ppp_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Create response semaphore for OTA control
    s_response_semaphore = xSemaphoreCreateBinary();
    if (!s_response_semaphore) {
        ESP_LOGE(TAG, "Failed to create response semaphore");
        vEventGroupDelete(s_ppp_event_group);
        return ESP_ERR_NO_MEM;
    }

    // Register event handler for PPP events
    esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &ppp_server_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // Enable NAPT on Wi-Fi interface for routing
    err = ppp_server_enable_napt(wifi_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable NAPT: %s (continuing)", esp_err_to_name(err));
    }

    // Create PPP server task
    // eppp_listen() blocks, so we need to run it in a separate task
    BaseType_t task_created = xTaskCreate(
        ppp_server_task,
        "ppp_server",
        PPP_SERVER_TASK_STACK_SIZE,
        NULL,
        PPP_SERVER_TASK_PRIORITY,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PPP server task");
        goto cleanup;
    }

    s_ppp_server_initialized = true;
    
    ESP_LOGI(TAG, "================================================================");
    ESP_LOGI(TAG, "PPP Server initialized successfully");
    ESP_LOGI(TAG, "Server task created, waiting for client connection...");
    ESP_LOGI(TAG, "================================================================");

    return ESP_OK;

cleanup:
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &ppp_server_event_handler);
    
    if (s_response_semaphore) {
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
    }
    if (s_ppp_event_group) {
        vEventGroupDelete(s_ppp_event_group);
        s_ppp_event_group = NULL;
    }
    return ESP_FAIL;
}

/**
 * @brief Deinitialize the PPP server
 */
esp_err_t ppp_server_deinit(void)
{
    if (!s_ppp_server_initialized) {
        return ESP_OK;
    }

    s_ppp_server_initialized = false;

    // Unregister event handler
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, 
                                 &ppp_server_event_handler);

    // Deinitialize eppp_link
    if (s_ppp_netif) {
        eppp_deinit(s_ppp_netif);
        s_ppp_netif = NULL;
    }

    // Cleanup synchronization objects
    if (s_response_semaphore) {
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
    }

    if (s_ppp_event_group) {
        vEventGroupDelete(s_ppp_event_group);
        s_ppp_event_group = NULL;
    }

    s_ppp_client_connected = false;
    
    ESP_LOGI(TAG, "PPP server deinitialized");

    return ESP_OK;
}
