/**
 * @file eth_connect.c
 * @brief Ethernet connection manager for WIZnet W5500 over SPI.
 *
 * Follows the same structure as wifi_connect.c / lte_connect.c:
 *  - Initialises hardware once on task start
 *  - Reports status via is_internet_connected and maintains eth_connected flag
 *  - Syncs time over SNTP and stores it to the PCF8563 RTC on first sync
 */

#include "eth_connect.h"
#include "config_handler.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "driver/spi_master.h"
#include "pcf8563_rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include "esp_eth_phy_w5500.h"
static const char *TAG = "ETH_CONNECT";

/* ── Module state ────────────────────────────────────────────────────────── */
esp_netif_t                  *g_eth_netif   = NULL;
static esp_eth_handle_t       s_eth_handle  = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static bool                   s_eth_connected = false;
static bool                   s_sntp_synced   = false;
static bool                   s_sntp_started  = false;
static volatile bool          s_task_running  = false;
static TaskHandle_t           s_task_handle   = NULL;

/* ── SNTP ─────────────────────────────────────────────────────────────────── */

static void sntp_sync_notification_cb(struct timeval *tv)
{
    if (!tv) return;

    ESP_LOGI(TAG, "SNTP time synchronised!");
    s_sntp_synced = true;

    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    esp_err_t ret = pcf8563_write_time(&timeinfo);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "System time synced to PCF8563 RTC");
        pcf8563_clear_voltage_low_flag();
        pcf8563_start();
    } else {
        ESP_LOGW(TAG, "Failed to sync time to PCF8563: %s", esp_err_to_name(ret));
    }
}

static void eth_init_sntp(void)
{
    if (s_sntp_started) return;

    ESP_LOGI(TAG, "Initialising SNTP (Ethernet)");

    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_init();

    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP initialised, waiting for sync...");
}

/* ── Event handler ────────────────────────────────────────────────────────── */

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up");
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            s_eth_connected  = false;
            is_internet_connected = false;
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet driver started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet driver stopped");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_eth_connected  = true;
        is_internet_connected = true;
        eth_init_sntp();
        /* HMI display will be updated on next hmi_refresh_status() call */
    }
}

/* ── SPI Ethernet hardware initialisation ───────────────────────────────── */

static esp_err_t eth_spi_hw_init(void)
{
    /* 1 ── Initialise SPI bus for Ethernet chip (W5500, DM9051, etc) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = ETH_SPI_MOSI_GPIO,
        .miso_io_num   = ETH_SPI_MISO_GPIO,
        .sclk_io_num   = ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SPI bus initialised (host=%d, SCK=%d, MOSI=%d, MISO=%d)",
             ETH_SPI_HOST, ETH_SPI_SCLK_GPIO, ETH_SPI_MOSI_GPIO, ETH_SPI_MISO_GPIO);

    /* 2 ── Create netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    g_eth_netif = esp_netif_new(&netif_cfg);
    if (!g_eth_netif) {
        ESP_LOGE(TAG, "Failed to create eth netif");
        spi_bus_free(ETH_SPI_HOST);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ethernet netif created");

    /* 3 ── Register event handlers BEFORE driver install */
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_EVENT handler");
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
        spi_bus_free(ETH_SPI_HOST);
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler");
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
        spi_bus_free(ETH_SPI_HOST);
        return ret;
    }
    ESP_LOGI(TAG, "Event handlers registered");

    /* 4 ── Install Ethernet driver
     * NOTE: When CONFIG_ETH_SPI_ETHERNET_W5500 (or similar) is set in sdkconfig,
     * the SPI bus setup above allows the driver to auto-detect and attach to the chip.
     * The MAC and PHY will be created automatically by the driver framework.
     */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(NULL, NULL);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: 0x%x", ret);
        ESP_LOGW(TAG, "Possible causes:");
        ESP_LOGW(TAG, "  - CONFIG_ETH_SPI_ETHERNET_W5500 not set in sdkconfig");
        ESP_LOGW(TAG, "  - SPI chip not detected on bus");
        ESP_LOGW(TAG, "  - GPIO pins incorrect");

        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
        spi_bus_free(ETH_SPI_HOST);
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet driver installed");

    /* 5 ── Attach netif glue */
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue) {
        ESP_LOGE(TAG, "Failed to create eth glue");
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
        spi_bus_free(ETH_SPI_HOST);
        return ESP_FAIL;
    }

    ret = esp_netif_attach(g_eth_netif, s_eth_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach eth netif: %s", esp_err_to_name(ret));
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
        spi_bus_free(ETH_SPI_HOST);
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet netif attached");

    /* 6 ── Start driver */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet (SPI) driver started — waiting for link...");
    return ESP_OK;
}

/* ── FreeRTOS task ────────────────────────────────────────────────────────── */

static void eth_task(void *arg)
{
    ESP_LOGI(TAG, "Ethernet task started");

    esp_err_t ret = eth_spi_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Ethernet hardware init failed (0x%x) — task exiting", ret);
        s_task_running = false;
        s_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_task_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Ethernet task stopping");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void eth_connect_task_start(void)
{
    if (s_task_running) {
        ESP_LOGW(TAG, "Ethernet task already running");
        return;
    }

    s_task_running  = true;
    s_eth_connected = false;
    s_sntp_synced   = false;

    BaseType_t ret = xTaskCreate(eth_task, "eth_task", 4096, NULL, 5,
                                 &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create eth_task");
        s_task_running = false;
        return;
    }

    ESP_LOGI(TAG, "Ethernet (W5500) connect started");
}

void eth_connect_task_stop(void)
{
    if (!s_task_running) {
        ESP_LOGW(TAG, "Ethernet task not running");
        return;
    }

    ESP_LOGI(TAG, "Stopping Ethernet connection...");
    s_task_running = false;

    /* Give the task time to exit its loop */
    if (s_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_task_handle = NULL;
    }

    /* Unregister event handlers */
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);

    /* Stop driver */
    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
    }

    /* Detach and destroy netif */
    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (g_eth_netif) {
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
    }

    /* Uninstall driver (also calls mac->del and phy->del internally) */
    if (s_eth_handle) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }

    spi_bus_free(ETH_SPI_HOST);

    s_eth_connected = false;
    is_internet_connected = false;
    /* HMI display will be updated on next hmi_refresh_status() call */

    ESP_LOGI(TAG, "Ethernet connection stopped");
}

bool eth_is_connected(void)   { return s_eth_connected; }
bool eth_is_sntp_synced(void) { return s_sntp_synced;   }
