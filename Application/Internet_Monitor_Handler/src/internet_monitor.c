/**
 * @file internet_monitor.c
 * @brief Internet connectivity monitor with automatic fallback switching.
 *
 * Periodically probes internet connectivity using a TCP connect attempt.
 * After INTERNET_MONITOR_FAIL_THRESHOLD consecutive failures on the primary
 * connection, it starts the fallback connection type.  When the primary
 * recovers the fallback is torn down and the primary is restarted.
 *
 * Fallback type is stored in g_internet_fallback_type (config_handler.c)
 * and is set automatically whenever the primary internet type changes:
 *   LTE / ETHERNET primary  →  fallback = WIFI
 *   WIFI primary            →  fallback = LTE (if APN set) else ETHERNET
 */

#include "internet_monitor.h"
#include "config_handler.h"
#include "lte_connect.h"
#include "wifi_connect.h"
#include "eth_connect.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>

static const char *TAG = "INET_MONITOR";

/* ── Globals declared in config_handler.c ────────────────────────────────── */
extern bool                    g_internet_fallback;
extern config_internet_type_t  g_internet_fallback_type;
extern config_internet_type_t  g_internet_type;
extern lte_config_context_t    g_lte_ctx;

/* ── Task state ──────────────────────────────────────────────────────────── */
static TaskHandle_t s_monitor_task_handle = NULL;
static volatile bool s_monitor_running    = false;
static volatile bool s_on_fallback        = false;

/* =========================================================================
 *  Private helpers
 * ====================================================================== */

/**
 * @brief Try TCP connect to INET_MONITOR_PROBE_HOST:PROBE_PORT.
 * @return true if TCP connect succeeded (internet reachable)
 */
bool internet_monitor_check_connectivity(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(INTERNET_MONITOR_PROBE_PORT);

    if (inet_pton(AF_INET, INTERNET_MONITOR_PROBE_HOST, &addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "inet_pton failed for " INTERNET_MONITOR_PROBE_HOST);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %d", errno);
        return false;
    }

    /* Set non-blocking SO_RCVTIMEO / SO_SNDTIMEO */
    struct timeval tv = { .tv_sec = INTERNET_MONITOR_PROBE_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    return (ret == 0);
}

/**
 * @brief Start one internet connection type.
 */
static void start_connection(config_internet_type_t type) {
    switch (type) {
    case CONFIG_INTERNET_WIFI:
        ESP_LOGI(TAG, "Starting WiFi connection");
        wifi_connect_task_start();
        break;
    case CONFIG_INTERNET_LTE:
        ESP_LOGI(TAG, "Starting LTE connection");
        lte_connect_task_start();
        break;
    case CONFIG_INTERNET_ETHERNET:
        ESP_LOGI(TAG, "Starting Ethernet connection");
        eth_connect_task_start();
        break;
    default:
        ESP_LOGW(TAG, "start_connection: unhandled type %d", type);
        break;
    }
}

/**
 * @brief Stop one internet connection type.
 */
static void stop_connection(config_internet_type_t type) {
    switch (type) {
    case CONFIG_INTERNET_WIFI:
        ESP_LOGI(TAG, "Stopping WiFi connection");
        wifi_connect_task_stop();
        break;
    case CONFIG_INTERNET_LTE:
        ESP_LOGI(TAG, "Stopping LTE connection");
        lte_connect_task_stop();
        break;
    case CONFIG_INTERNET_ETHERNET:
        ESP_LOGI(TAG, "Stopping Ethernet connection");
        eth_connect_task_stop();
        break;
    default:
        break;
    }
}

/* =========================================================================
 *  Monitor Task
 * ====================================================================== */

static void internet_monitor_task(void *arg) {
    ESP_LOGI(TAG, "Internet monitor task started (primary=%d, fallback=%d)",
             g_internet_type, g_internet_fallback_type);

    int fail_count     = 0;
    int recover_count  = 0;
    const int RECOVER_THRESHOLD = 3; /* successes needed to switch back */

    while (s_monitor_running) {
        /* Sleep between checks */
        for (int i = 0; i < (INTERNET_MONITOR_CHECK_INTERVAL_MS / 1000) && s_monitor_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!s_monitor_running) break;

        bool ok = internet_monitor_check_connectivity();

        if (!s_on_fallback) {
            /* ── Monitoring primary ─────────────────────────────────────── */
            if (ok) {
                fail_count = 0;
                ESP_LOGD(TAG, "Primary internet OK");
            } else {
                fail_count++;
                ESP_LOGW(TAG, "Primary internet FAIL (%d/%d)",
                         fail_count, INTERNET_MONITOR_FAIL_THRESHOLD);

                if (fail_count >= INTERNET_MONITOR_FAIL_THRESHOLD) {
                    ESP_LOGW(TAG, "Primary failed %d times — switching to fallback type %d",
                             fail_count, g_internet_fallback_type);

                    /* Switch to fallback */
                    stop_connection(g_internet_type);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    start_connection(g_internet_fallback_type);
                    s_on_fallback = true;
                    fail_count    = 0;
                    recover_count = 0;
                }
            }
        } else {
            /* ── On fallback — check if primary has recovered ───────────── */
            /* Re-check connectivity; if it fails, the fallback is keeping
             * us alive (or fallback also failed — logged below) */
            if (ok) {
                recover_count++;
                ESP_LOGI(TAG, "Connectivity OK on fallback (%d/%d stable)",
                         recover_count, RECOVER_THRESHOLD);
            } else {
                recover_count = 0;
                ESP_LOGW(TAG, "Connectivity FAIL even on fallback — no internet");
            }

            /* Optionally: after a long period on fallback, try switching back.
             * For simplicity we attempt to restore primary after 3 × check intervals
             * of stable connectivity (giving the primary time to recover). */
            if (recover_count >= RECOVER_THRESHOLD) {
                ESP_LOGI(TAG, "Attempting to restore primary internet (type %d)", g_internet_type);
                stop_connection(g_internet_fallback_type);
                vTaskDelay(pdMS_TO_TICKS(2000));
                start_connection(g_internet_type);
                s_on_fallback = false;
                fail_count    = 0;
                recover_count = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Internet monitor task stopped");
    s_monitor_task_handle = NULL;
    vTaskDelete(NULL);
}

/* =========================================================================
 *  Public API
 * ====================================================================== */

void internet_monitor_task_start(void) {
    if (!g_internet_fallback) {
        ESP_LOGI(TAG, "Fallback disabled — monitor not started");
        return;
    }
    if (s_monitor_running) {
        ESP_LOGW(TAG, "Monitor task already running");
        return;
    }

    s_monitor_running = true;
    s_on_fallback     = false;

    BaseType_t ret = xTaskCreate(internet_monitor_task,
                                 "inet_monitor",
                                 4096,
                                 NULL,
                                 3,
                                 &s_monitor_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create internet monitor task");
        s_monitor_running = false;
    } else {
        ESP_LOGI(TAG, "Internet monitor task created");
    }
}

void internet_monitor_task_stop(void) {
    if (!s_monitor_running) return;
    s_monitor_running = false;
    /* Task will self-delete when it sees s_monitor_running = false */
    ESP_LOGI(TAG, "Internet monitor task stop requested");
}
