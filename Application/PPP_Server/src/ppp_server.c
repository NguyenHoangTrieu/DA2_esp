/*
 * PPP Server Component for WAN MCU
 * 
 * This component implements a PPP server that shares the Wi-Fi connection
 * with the LAN MCU via UART. It also manages OTA update flow control.
 */

#include "ppp_server.h"
#include "ppp_server_config.h"
#include "fota_handler.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "lwip/lwip_napt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "ppp_server";

/* PPP Server State */
static esp_netif_t *s_ppp_netif = NULL;
static esp_netif_t *s_wifi_netif = NULL;
static bool s_ppp_server_initialized = false;

/* UART Event Queue */
static QueueHandle_t s_uart_queue = NULL;

/* Response tracking */
static SemaphoreHandle_t s_response_semaphore = NULL;
static esp_err_t s_last_response_result = ESP_ERR_TIMEOUT;

/* Function Prototypes */
static esp_err_t ppp_server_uart_init(void);
static void ppp_server_uart_event_task(void *pvParameters);
static esp_err_t ppp_server_enable_napt(esp_netif_t *wifi_netif);
static void process_uart_response(const char *data, size_t len);

/**
 * @brief Initialize UART for PPP communication
 */
static esp_err_t ppp_server_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = PPP_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        uart_driver_install(PPP_UART_PORT, PPP_UART_BUF_SIZE, PPP_UART_BUF_SIZE, 
                           PPP_UART_QUEUE_SIZE, &s_uart_queue, 0),
        TAG, "Failed to install UART driver");

    ESP_RETURN_ON_ERROR(
        uart_param_config(PPP_UART_PORT, &uart_config),
        TAG, "Failed to configure UART parameters");

    ESP_RETURN_ON_ERROR(
        uart_set_pin(PPP_UART_PORT, PPP_UART_TX_PIN, PPP_UART_RX_PIN, 
                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "Failed to set UART pins");

    ESP_LOGI(TAG, "UART initialized: Port %d, Baud %d, TX %d, RX %d",
             PPP_UART_PORT, PPP_UART_BAUD_RATE, PPP_UART_TX_PIN, PPP_UART_RX_PIN);

    return ESP_OK;
}

/**
 * @brief Process UART response from LAN MCU
 */
static void process_uart_response(const char *data, size_t len)
{
    if (len >= PPP_RESPONSE_OK_LEN && 
        strncmp(data, PPP_RESPONSE_OK_CMD, PPP_RESPONSE_OK_LEN) == 0) {
        
        ESP_LOGI(TAG, "Received OTA_LAN_OK from LAN MCU");
        s_last_response_result = ESP_OK;
        
        if (s_response_semaphore) {
            xSemaphoreGive(s_response_semaphore);
        }
        
    } else if (len >= PPP_RESPONSE_FAIL_LEN && 
               strncmp(data, PPP_RESPONSE_FAIL_CMD, PPP_RESPONSE_FAIL_LEN) == 0) {
        
        ESP_LOGE(TAG, "Received OTA_LAN_FAIL from LAN MCU");
        s_last_response_result = ESP_FAIL;
        
        if (s_response_semaphore) {
            xSemaphoreGive(s_response_semaphore);
        }
    }
}

/**
 * @brief UART event task to monitor responses from LAN MCU
 */
static void ppp_server_uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *data_buf = (uint8_t *)malloc(PPP_RESPONSE_BUF_SIZE);
    
    if (!data_buf) {
        ESP_LOGE(TAG, "Failed to allocate UART data buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_uart_queue, (void *)&event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    if (event.size > 0 && event.size < PPP_RESPONSE_BUF_SIZE) {
                        int len = uart_read_bytes(PPP_UART_PORT, data_buf, 
                                                 event.size, portMAX_DELAY);
                        if (len > 0) {
                            data_buf[len] = '\0';
                            ESP_LOGD(TAG, "UART RX: %s", data_buf);
                            process_uart_response((char *)data_buf, len);
                        }
                    }
                    break;

                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(PPP_UART_PORT);
                    xQueueReset(s_uart_queue);
                    break;

                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full");
                    uart_flush_input(PPP_UART_PORT);
                    xQueueReset(s_uart_queue);
                    break;

                case UART_BREAK:
                    ESP_LOGD(TAG, "UART break detected");
                    break;

                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    break;

                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;

                default:
                    ESP_LOGD(TAG, "UART event type: %d", event.type);
                    break;
            }
        }
    }

    free(data_buf);
    vTaskDelete(NULL);
}

/**
 * @brief Enable NAPT on the Wi-Fi interface
 */
static esp_err_t ppp_server_enable_napt(esp_netif_t *wifi_netif)
{
    if (!wifi_netif) {
        ESP_LOGE(TAG, "Invalid wifi_netif");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ip_napt_enable(_lwip_netif_get_netif_impl_index(wifi_netif), 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable NAPT on Wi-Fi interface: %s", 
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NAPT enabled on Wi-Fi interface");
    return ESP_OK;
}

/**
 * @brief Initialize the PPP server
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

    s_wifi_netif = wifi_netif;

    // Create response semaphore
    s_response_semaphore = xSemaphoreCreateBinary();
    if (!s_response_semaphore) {
        ESP_LOGE(TAG, "Failed to create response semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Initialize UART
    esp_err_t err = ppp_server_uart_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
        return err;
    }

    // Create PPP server netif
    esp_netif_config_t ppp_netif_config = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&ppp_netif_config);
    if (!s_ppp_netif) {
        ESP_LOGE(TAG, "Failed to create PPP netif");
        uart_driver_delete(PPP_UART_PORT);
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
        return ESP_FAIL;
    }

    // Attach PPP to UART
    void *ppp_netif_driver = esp_netif_ppp_create_server_over_uart(
        PPP_UART_PORT, s_ppp_netif);
    
    if (!ppp_netif_driver) {
        ESP_LOGE(TAG, "Failed to create PPP server driver");
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
        uart_driver_delete(PPP_UART_PORT);
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
        return ESP_FAIL;
    }

    // Enable NAPT on Wi-Fi interface
    err = ppp_server_enable_napt(wifi_netif);
    if (err != ESP_OK) {
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
        uart_driver_delete(PPP_UART_PORT);
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
        return err;
    }

    // Start UART event monitoring task
    BaseType_t task_created = xTaskCreate(
        ppp_server_uart_event_task,
        "ppp_uart_event",
        PPP_SERVER_TASK_STACK_SIZE,
        NULL,
        PPP_SERVER_TASK_PRIORITY,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART event task");
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
        uart_driver_delete(PPP_UART_PORT);
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
        return ESP_FAIL;
    }

    s_ppp_server_initialized = true;
    ESP_LOGI(TAG, "PPP server initialized successfully");

    return ESP_OK;
}

/**
 * @brief Trigger OTA update on the LAN MCU
 */
esp_err_t ppp_server_trigger_lan_ota(void)
{
    if (!s_ppp_server_initialized) {
        ESP_LOGE(TAG, "PPP server not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending OTA trigger command to LAN MCU: %s", PPP_TRIGGER_CMD);

    // Reset response tracking
    s_last_response_result = ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_response_semaphore, 0); // Clear any previous signal

    // Send trigger command via UART
    int len = uart_write_bytes(PPP_UART_PORT, PPP_TRIGGER_CMD, PPP_TRIGGER_CMD_LEN);
    if (len != PPP_TRIGGER_CMD_LEN) {
        ESP_LOGE(TAG, "Failed to send OTA trigger command");
        return ESP_FAIL;
    }

    // Wait for transmission to complete
    ESP_RETURN_ON_ERROR(
        uart_wait_tx_done(PPP_UART_PORT, pdMS_TO_TICKS(1000)),
        TAG, "UART TX timeout");

    ESP_LOGI(TAG, "OTA trigger command sent successfully");
    return ESP_OK;
}

/**
 * @brief Wait for response from LAN MCU
 */
esp_err_t ppp_server_wait_for_lan_response(TickType_t timeout)
{
    if (!s_ppp_server_initialized) {
        ESP_LOGE(TAG, "PPP server not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Waiting for response from LAN MCU (timeout: %lu ms)", 
             pdTICKS_TO_MS(timeout));

    // Wait for semaphore signal from UART task
    if (xSemaphoreTake(s_response_semaphore, timeout) == pdTRUE) {
        ESP_LOGI(TAG, "Received response from LAN MCU: %s", 
                 s_last_response_result == ESP_OK ? "SUCCESS" : "FAILURE");
        return s_last_response_result;
    }

    ESP_LOGE(TAG, "Timeout waiting for LAN MCU response");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Deinitialize the PPP server
 */
esp_err_t ppp_server_deinit(void)
{
    if (!s_ppp_server_initialized) {
        return ESP_OK;
    }

    if (s_ppp_netif) {
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
    }

    uart_driver_delete(PPP_UART_PORT);

    if (s_response_semaphore) {
        vSemaphoreDelete(s_response_semaphore);
        s_response_semaphore = NULL;
    }

    s_ppp_server_initialized = false;
    ESP_LOGI(TAG, "PPP server deinitialized");

    return ESP_OK;
}
