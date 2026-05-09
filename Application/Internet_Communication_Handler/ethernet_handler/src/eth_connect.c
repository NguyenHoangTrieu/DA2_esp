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
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "driver/spi_master.h"
#include "mcu_lan_handler.h"
#include "pcf8563_rtc.h"
#include "stack_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>

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

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static esp_err_t eth_set_default_mac(esp_eth_handle_t eth_handle);

#define ETH_ADAPTER_STACK_ID          1
#define ETH_ADAPTER_MODULE_ID         "015"
#define ETH_ADAPTER_POWER_PIN         STACK_GPIO_PIN_04
#define ETH_ADAPTER_RESET_PIN         STACK_GPIO_PIN_05
#define ETH_ADAPTER_INT_PIN           STACK_GPIO_PIN_06
#define ETH_ADAPTER_RESET_INACTIVE    1
#define ETH_ADAPTER_RESET_ACTIVE      0
#define ETH_ADAPTER_POWER_SETTLE_MS   20
#define ETH_ADAPTER_RESET_ACTIVE_MS   10
#define ETH_ADAPTER_RESET_READY_MS    120

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

static void eth_release_resources(bool unregister_events)
{
    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_eth_handle) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }

    if (unregister_events) {
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler);
    }

    if (g_eth_netif) {
        esp_netif_destroy(g_eth_netif);
        g_eth_netif = NULL;
    }

    spi_bus_free(ETH_SPI_HOST);
}

static esp_err_t eth_set_default_mac(esp_eth_handle_t eth_handle)
{
    uint8_t base_mac[6] = {0};
    uint8_t eth_mac[6] = {0};

    esp_err_t ret = esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read base MAC: %s", esp_err_to_name(ret));
        return ret;
    }

    memcpy(eth_mac, base_mac, sizeof(eth_mac));
    eth_mac[0] |= 0x02;
    eth_mac[0] &= 0xFE;
    eth_mac[5] ^= 0x01;

    ret = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Ethernet MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "W5500 MAC address set to %02X:%02X:%02X:%02X:%02X:%02X",
             eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
    return ESP_OK;
}

static esp_err_t eth_prepare_adapter(void)
{
    const char *module_id = stack_handler_get_module_id(ETH_ADAPTER_STACK_ID);
    if (strcmp(module_id, ETH_ADAPTER_MODULE_ID) != 0) {
        ESP_LOGE(TAG, "Ethernet adapter not detected on slot %d (module_id=%s)",
                 ETH_ADAPTER_STACK_ID, module_id);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = stack_handler_gpio_set_direction(ETH_ADAPTER_STACK_ID,
                                                     ETH_ADAPTER_POWER_PIN,
                                                     true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure adapter power pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = stack_handler_gpio_set_direction(ETH_ADAPTER_STACK_ID,
                                           ETH_ADAPTER_RESET_PIN,
                                           true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure adapter reset pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = stack_handler_gpio_set_direction(ETH_ADAPTER_STACK_ID,
                                           ETH_ADAPTER_INT_PIN,
                                           false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure adapter INT pin as input: %s", esp_err_to_name(ret));
    }

    ret = stack_handler_gpio_write(ETH_ADAPTER_STACK_ID,
                                   ETH_ADAPTER_POWER_PIN,
                                   true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable adapter 3V3 rail: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = stack_handler_gpio_write(ETH_ADAPTER_STACK_ID,
                                   ETH_ADAPTER_RESET_PIN,
                                   ETH_ADAPTER_RESET_INACTIVE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set adapter reset inactive: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(ETH_ADAPTER_POWER_SETTLE_MS));

    ret = stack_handler_gpio_write(ETH_ADAPTER_STACK_ID,
                                   ETH_ADAPTER_RESET_PIN,
                                   ETH_ADAPTER_RESET_ACTIVE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to assert adapter reset: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(ETH_ADAPTER_RESET_ACTIVE_MS));

    ret = stack_handler_gpio_write(ETH_ADAPTER_STACK_ID,
                                   ETH_ADAPTER_RESET_PIN,
                                   ETH_ADAPTER_RESET_INACTIVE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release adapter reset: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(ETH_ADAPTER_RESET_READY_MS));
    ESP_LOGI(TAG, "Ethernet adapter prepared via IOX (P04=power, P05=reset#, P06=int#)");
    return ESP_OK;
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
            mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
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
        mcu_lan_handler_set_internet_status(INTERNET_STATUS_ONLINE);
        eth_init_sntp();
        /* HMI display will be updated on next hmi_refresh_status() call */
    }
}

/* ── SPI Ethernet hardware initialisation ───────────────────────────────── */

static esp_err_t eth_spi_hw_init(void)
{
    esp_err_t ret = eth_prepare_adapter();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 1 ── Initialise SPI bus for Ethernet chip (W5500, DM9051, etc) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = ETH_SPI_MOSI_GPIO,
        .miso_io_num   = ETH_SPI_MISO_GPIO,
        .sclk_io_num   = ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ret = spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
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
        eth_release_resources(false);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ethernet netif created");

    /* 3 ── Register event handlers BEFORE driver install */
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_EVENT handler");
        eth_release_resources(false);
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler");
        eth_release_resources(true);
        return ret;
    }
    ESP_LOGI(TAG, "Event handlers registered");

    /* 4 ── Create the actual W5500 MAC/PHY instances required by ESP-IDF. */
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 16,
        .spics_io_num = ETH_SPI_CS_GPIO,
    };
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
    w5500_cfg.int_gpio_num = ETH_INT_GPIO;
    if (ETH_INT_GPIO < 0) {
        w5500_cfg.poll_period_ms = 100;
        ESP_LOGI(TAG, "W5500 INT# is behind IOX, using polling mode (%d ms)",
                 w5500_cfg.poll_period_ms);
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC");
        eth_release_resources(true);
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        mac->del(mac);
        eth_release_resources(true);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: 0x%x", ret);
        ESP_LOGW(TAG, "Possible causes:");
        ESP_LOGW(TAG, "  - SPI chip not detected on bus");
        ESP_LOGW(TAG, "  - GPIO pins incorrect");
        ESP_LOGW(TAG, "  - W5500 INT/RST wiring mismatch");

        phy->del(phy);
        mac->del(mac);
        eth_release_resources(true);
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet driver installed");

    ret = eth_set_default_mac(s_eth_handle);
    if (ret != ESP_OK) {
        eth_release_resources(true);
        return ret;
    }

    /* 5 ── Attach netif glue */
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue) {
        ESP_LOGE(TAG, "Failed to create eth glue");
        eth_release_resources(true);
        return ESP_FAIL;
    }

    ret = esp_netif_attach(g_eth_netif, s_eth_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach eth netif: %s", esp_err_to_name(ret));
        eth_release_resources(true);
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet netif attached");

    /* 6 ── Start driver */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
        eth_release_resources(true);
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
    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
    }
    eth_release_resources(true);

    s_eth_connected = false;
    is_internet_connected = false;
    mcu_lan_handler_set_internet_status(INTERNET_STATUS_OFFLINE);
    /* HMI display will be updated on next hmi_refresh_status() call */

    ESP_LOGI(TAG, "Ethernet connection stopped");
}

bool eth_is_connected(void)   { return s_eth_connected; }
bool eth_is_sntp_synced(void) { return s_sntp_synced;   }
