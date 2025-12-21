#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "usbh_modem_board.h"
#include "usbh_modem_wifi.h"

/* ===== Configuration Defines ===== */
/* Board selection */
#define ESP32_S3_GENERIC                1

/* Web router configuration */
#define EXAMPLE_ENABLE_WEB_ROUTER       0    /* Disable web router */
#define EXAMPLE_WEB_MOUNT_POINT         "/spiffs"
#define EXAMPLE_WEB_USERNAME            "esp32"
#define EXAMPLE_WEB_PASSWORD            "12345678"

/* Modem configuration */
#define EXAMPLE_ENTER_PPP_DURING_INIT   1    /* Auto enter PPP mode */
#define EXAMPLE_AUTO_UPDATE_DNS         1    /* Auto update DNS */
#define EXAMPLE_PING_NETWORK            1    /* Enable ping */
#define EXAMPLE_PING_MANUAL             1    /* Ping manual address */
#define EXAMPLE_PING_MANUAL_ADDR        "8.8.8.8"
#define EXAMPLE_PING_TIMEOUT            2000 /* ms */

/* Debug configuration */
#define DUMP_SYSTEM_STATUS              0    /* Disable system dump */

/* ===== End Configuration ===== */

#if EXAMPLE_ENABLE_WEB_ROUTER
#include "modem_http_config.h"
#endif

#if EXAMPLE_PING_NETWORK
#include "ping/ping_sock.h"
#endif

static const char *TAG = "4g_main";

static modem_wifi_config_t s_modem_wifi_config = MODEM_WIFI_DEFAULT_CONFIG();

#if DUMP_SYSTEM_STATUS
#define TASK_MAX_COUNT 32
typedef struct {
    uint32_t ulRunTimeCounter;
    uint32_t xTaskNumber;
} taskData_t;

static taskData_t previousSnapshot[TASK_MAX_COUNT];
static int taskTopIndex = 0;
static uint32_t previousTotalRunTime = 0;

static taskData_t *getPreviousTaskData(uint32_t xTaskNumber)
{
    for (int i = 0; i < taskTopIndex; i++) {
        if (previousSnapshot[i].xTaskNumber == xTaskNumber) {
            return &previousSnapshot[i];
        }
    }
    ESP_ERROR_CHECK(!(taskTopIndex < TASK_MAX_COUNT));
    taskData_t *result = &previousSnapshot[taskTopIndex];
    result->xTaskNumber = xTaskNumber;
    taskTopIndex++;
    return result;
}

static void _system_dump()
{
    uint32_t totalRunTime;
    TaskStatus_t taskStats[TASK_MAX_COUNT];
    uint32_t taskCount = uxTaskGetSystemState(taskStats, TASK_MAX_COUNT, &totalRunTime);
    ESP_ERROR_CHECK(!(taskTopIndex < TASK_MAX_COUNT));
    uint32_t totalDelta = totalRunTime - previousTotalRunTime;
    float f = 100.0 / totalDelta;

    ESP_LOGI(TAG, "Task dump\n");
    ESP_LOGI(TAG, "Load\tStack left\tName\tPRI\n");

    for (uint32_t i = 0; i < taskCount; i++) {
        TaskStatus_t *stats = &taskStats[i];
        taskData_t *previousTaskData = getPreviousTaskData(stats->xTaskNumber);
        uint32_t taskRunTime = stats->ulRunTimeCounter;
        float load = f * (taskRunTime - previousTaskData->ulRunTimeCounter);
        ESP_LOGI(TAG, "%.2f \t%" PRIu32 "\t%s %" PRIu32 "\t\n", 
                 load, stats->usStackHighWaterMark, stats->pcTaskName, 
                 (uint32_t)stats->uxBasePriority);
        previousTaskData->ulRunTimeCounter = taskRunTime;
    }
    
    ESP_LOGI(TAG, "Free heap=%d Free mini=%d bigst=%d, internal=%d bigst=%d",
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT), 
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    previousTotalRunTime = totalRunTime;
}
#endif

static void on_modem_event(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
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

#if EXAMPLE_PING_NETWORK
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ESP_LOGI(TAG, "%"PRIu32" bytes from %s icmp_seq=%u ttl=%u time=%"PRIu32" ms", 
             recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, elapsed_time);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGW(TAG, "From %s icmp_seq=%u timeout", ipaddr_ntoa(&target_addr), seqno);
}
#endif

void app_main(void)
{
#ifdef CONFIG_ESP32_S3_USB_OTG
    bsp_usb_mode_select_host();
    bsp_usb_host_power_mode(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
#endif

    /* Initialize NVS for Wi-Fi storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize default TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Banner */
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "     ESP 4G Cat.1 Wi-Fi Router");
    ESP_LOGI(TAG, "====================================");

    /* Initialize modem board - using MODEM_DEFAULT_CONFIG() to avoid memory issues */
    modem_config_t modem_config = MODEM_DEFAULT_CONFIG();
    
#if !EXAMPLE_ENTER_PPP_DURING_INIT
    modem_config.flags |= MODEM_FLAGS_INIT_NOT_ENTER_PPP;
    modem_config.flags |= MODEM_FLAGS_INIT_NOT_BLOCK;
#endif
    modem_config.handler = on_modem_event;
    
    ret = modem_board_init(&modem_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Modem init failed: 0x%x", ret);
        return;
    }
    ESP_LOGI(TAG, "Modem initialized successfully");

#if EXAMPLE_ENABLE_WEB_ROUTER
    modem_http_get_nvs_wifi_config(&s_modem_wifi_config);
    modem_http_init(&s_modem_wifi_config);
#endif

    esp_netif_t *ap_netif = modem_wifi_ap_init();
    assert(ap_netif != NULL);
    ESP_ERROR_CHECK(modem_wifi_set(&s_modem_wifi_config));

#if EXAMPLE_PING_NETWORK
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    char *ping_addr_s = NULL;
    
#if EXAMPLE_PING_MANUAL
    ping_addr_s = EXAMPLE_PING_MANUAL_ADDR;
#else
    esp_netif_dns_info_t dns2;
    modem_board_get_dns_info(ESP_NETIF_DNS_MAIN, &dns2);
    ping_addr_s = ip4addr_ntoa((ip4_addr_t *)(&dns2.ip.u_addr.ip4));
#endif

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ipaddr_aton(ping_addr_s, &target_addr);
    ping_config.target_addr = target_addr;
    ping_config.timeout_ms = EXAMPLE_PING_TIMEOUT;
    ping_config.task_stack_size = 4096;
    ping_config.count = 1;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };
    esp_ping_handle_t ping;
    esp_ping_new_session(&ping_config, &cbs, &ping);
#endif

#if EXAMPLE_AUTO_UPDATE_DNS
    uint32_t ap_dns_addr = 0;
#endif

    /* Main loop */
    while (1) {
        /* Get signal quality */
        int rssi = 0, ber = 0;
        modem_board_get_signal_quality(&rssi, &ber);
        ESP_LOGI(TAG, "Signal - RSSI: %d dBm, BER: %d", rssi, ber);

#if EXAMPLE_AUTO_UPDATE_DNS
        esp_netif_dns_info_t dns;
        modem_board_get_dns_info(ESP_NETIF_DNS_MAIN, &dns);
        uint32_t _ap_dns_addr = dns.ip.u_addr.ip4.addr;
        if (_ap_dns_addr != ap_dns_addr) {
            modem_wifi_set_dns(ap_netif, _ap_dns_addr);
            ap_dns_addr = _ap_dns_addr;
            ESP_LOGI(TAG, "DNS updated: %s", inet_ntoa(ap_dns_addr));
        }
#endif

#if EXAMPLE_PING_NETWORK
        ESP_LOGI(TAG, "Pinging %s...", ping_addr_s);
        esp_ping_start(ping);
#endif

#if DUMP_SYSTEM_STATUS
        _system_dump();
#endif
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    
    modem_board_deinit();
}
