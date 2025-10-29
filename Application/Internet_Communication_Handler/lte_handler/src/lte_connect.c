#include "lte_connect.h"
#include "lte_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// Default APN/credentials for LTE (customize per carrier/region)
#define DEFAULT_LTE_APN         "v-internet"
#define DEFAULT_LTE_USERNAME    ""
#define DEFAULT_LTE_PASSWORD    ""

// Auto-reconnect configuration
#define LTE_RECONNECT_TIMEOUT_MS    30000
#define LTE_MAX_RECONNECT_ATTEMPTS  0    // 0 means infinite attempts

static TaskHandle_t lte_config_task_handle = NULL;
static volatile bool lte_connect_task_close = false;

// Internal configuration store for APN/auth
static char s_lte_apn[64] = DEFAULT_LTE_APN;
static char s_lte_username[32] = DEFAULT_LTE_USERNAME;
static char s_lte_password[32] = DEFAULT_LTE_PASSWORD;

// Internal connection status flags
static bool s_lte_connected = false;
static bool s_lte_initialized = false;

/**
 * @brief Initializes the LTE handler and attempts connection.
 * If already initialized, performs a clean reinitialization.
 */
static esp_err_t lte_init_with_config(const char *apn, const char *username, const char *password)
{
    if (s_lte_initialized) {
        lte_handler_disconnect();
        lte_handler_deinit();
        s_lte_initialized = false;
        vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to ensure proper shutdown
    }

    lte_handler_config_t lte_config = LTE_HANDLER_CONFIG_DEFAULT();
    lte_config.apn = apn;
    lte_config.username = username;
    lte_config.password = password;
    lte_config.auto_reconnect = true;
    lte_config.reconnect_timeout_ms = LTE_RECONNECT_TIMEOUT_MS;
    lte_config.max_reconnect_attempts = LTE_MAX_RECONNECT_ATTEMPTS;
    esp_err_t ret = lte_handler_init(&lte_config);

    if (ret == ESP_OK) {
        s_lte_initialized = true;
        ret = lte_handler_connect();
        if (ret == ESP_OK) {
            // Connected successfully (status flag set)
            s_lte_connected = true;
        }
    }
    return ret;
}

/**
 * @brief Background task for modem connection monitoring and recovery.
 * Periodically polls connection status, attempts auto-reconnect if lost,
 * and prints signal quality information.
 */
static void lte_config_task(void *arg)
{
    uint32_t monitor_counter = 0;
    while (!lte_connect_task_close) {
        // Poll LTE connection state periodically
        if (s_lte_initialized && (++monitor_counter % 20 == 0)) {
            if (!lte_handler_is_connected()) {
                s_lte_connected = false;
                // Attempt automatic reconnect
                lte_handler_disconnect();
                lte_handler_connect();
            } else {
                s_lte_connected = true;
            }
        }

        // Periodically query and print signal strength
        if (s_lte_connected && (monitor_counter % 50 == 0)) {
            uint32_t rssi, ber;
            if (lte_handler_get_signal_strength(&rssi, &ber) == ESP_OK) {
                printf("LTE Signal Quality: RSSI=%lu, BER=%lu\n", rssi, ber);
            }
        }

        // EXTENSION: Here you could handle config updates from a queue

        vTaskDelay(pdMS_TO_TICKS(200)); // Run every 200 ms
    }
    vTaskDelete(NULL);
}

//====== PUBLIC API IMPLEMENTATIONS ======

/**
 * @brief Starts the LTE connection background task.
 * This initializes and connects the modem and begins periodic management.
 */
void lte_connect_task_start(void)
{
    if (lte_config_task_handle != NULL)
        return; // Already running

    lte_connect_task_close = false;
    s_lte_connected = false;
    s_lte_initialized = false;
    // Initialize LTE handler with stored config
    lte_init_with_config(s_lte_apn, s_lte_username, s_lte_password);
    // Start monitoring task
    xTaskCreate(lte_config_task, "lte_config_task", 4096, NULL, 5, &lte_config_task_handle);
}

/**
 * @brief Stops the LTE connection background task, disconnects, deinitializes modem.
 */
void lte_connect_task_stop(void)
{
    if (lte_connect_task_close) {
        return;
    }
    lte_connect_task_close = true;
    if (lte_config_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(500)); // Allow task cleanup
        lte_config_task_handle = NULL;
    }
    if (s_lte_initialized) {
        if (s_lte_connected) {
            lte_handler_disconnect();
        }
        lte_handler_deinit();
        s_lte_initialized = false;
        s_lte_connected = false;
    }
}

/**
 * @brief Returns whether the modem is currently connected to LTE network.
 */
bool lte_is_connected(void)
{
    return s_lte_connected && lte_handler_is_connected();
}

/**
 * @brief Fetches the LTE signal quality (RSSI/BER) from the modem.
 */
esp_err_t lte_get_signal_strength(uint32_t *rssi, uint32_t *ber)
{
    if (!s_lte_initialized)
        return ESP_ERR_INVALID_STATE;
    return lte_handler_get_signal_strength(rssi, ber);
}
