/**
 * @file hmi_task.c
 * @brief HMI Application Task — FreeRTOS task for HMI mode management
 *
 * Implements the top layer of the three-tier HMI stack:
 *   - Mode FSM (inactive ↔ active)
 *   - UART switch control (GPIO46: 0=LAN MCU path, 1=HMI display path)
 *   - RX FreeRTOS task: reads TJC frames and dispatches to hmi_display layer
 *   - Thread-safe status cache and display refresh
 */

#include "hmi_task.h"
#include "hmi_display.h"
#include "hmi_handler.h"        /* BSP: hmi_bsp_init / deinit / read_frame */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "HMI_TASK";

/* Forward declarations for UART switch helpers defined in DA2_esp.c */
extern void uart_switch_route_to_hmi(void);
extern void uart_switch_route_to_lan_mcu(void);

/* ------------------------------------------------------------------ */
/*  Task configuration                                                  */
/* ------------------------------------------------------------------ */
#define HMI_RX_TASK_STACK    4096   /* bytes of stack for RX task     */
#define HMI_RX_TASK_PRIO     5      /* FreeRTOS priority              */

/* ------------------------------------------------------------------ */
/*  Module state                                                        */
/* ------------------------------------------------------------------ */
static volatile bool     s_active      = false;
static volatile bool     s_rx_running  = false;
static TaskHandle_t      s_rx_task     = NULL;
static StackType_t      *s_rx_stack    = NULL;
static StaticTask_t     *s_rx_tcb      = NULL;
static SemaphoreHandle_t s_status_mutex  = NULL;
static SemaphoreHandle_t s_display_mutex = NULL;
static hmi_status_t      s_cached      = {0};

static bool hmi_task_lock_display(TickType_t timeout)
{
    return s_display_mutex && xSemaphoreTake(s_display_mutex, timeout) == pdTRUE;
}

static void hmi_task_unlock_display(void)
{
    if (s_display_mutex) {
        xSemaphoreGive(s_display_mutex);
    }
}

static void hmi_task_free_rx_buffers(void)
{
    if (s_rx_stack) {
        heap_caps_free(s_rx_stack);
        s_rx_stack = NULL;
    }
    if (s_rx_tcb) {
        heap_caps_free(s_rx_tcb);
        s_rx_tcb = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  RX task                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Reads TJC event frames from UART2 and dispatches them to the
 *        Middleware layer (hmi_display_handle_frame).
 *        Runs until s_active is set to false.
 */
static void rx_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");
    s_rx_running = true;

    uint8_t frame[256];
    while (s_active) {
        int len = hmi_bsp_read_frame(frame, sizeof(frame), 200);
        if (len > 0) {
            if (hmi_task_lock_display(portMAX_DELAY)) {
                hmi_display_handle_frame(frame, len);
                hmi_task_unlock_display();
            }
        }
        /* Display refresh is driven externally by hmi_task_update_status()
         * which is called by pwr_monitor_task every 5 seconds.            */
    }

    s_rx_running = false;
    ESP_LOGI(TAG, "RX task stopped");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void hmi_task_init(void)
{
    s_active     = false;
    s_rx_running = false;
    s_rx_task    = NULL;
    hmi_task_free_rx_buffers();
    memset(&s_cached, 0, sizeof(s_cached));

    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
    }
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateMutex();
    }
    if (!s_status_mutex || !s_display_mutex) {
        ESP_LOGE(TAG, "Failed to create HMI mutexes");
    }
    hmi_display_init();
    ESP_LOGI(TAG, "HMI task initialised (inactive)");
}

esp_err_t hmi_task_enter_mode(void)
{
    esp_err_t ret = ESP_OK;

    if (s_active) {
        ESP_LOGW(TAG, "Already in HMI mode");
        return ESP_OK;
    }
    if (!s_display_mutex || !s_status_mutex) {
        ESP_LOGE(TAG, "HMI task not initialised correctly");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Route UART2 switch to HMI display path (GPIO46 HIGH) */
    uart_switch_route_to_hmi();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Install UART2 BSP driver */
    ret = hmi_bsp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %s", esp_err_to_name(ret));
        uart_switch_route_to_lan_mcu();
        return ret;
    }

    if (!hmi_task_lock_display(portMAX_DELAY)) {
        hmi_bsp_deinit();
        uart_switch_route_to_lan_mcu();
        return ESP_ERR_INVALID_STATE;
    }

    /* 3. Reset Middleware state before RX task is allowed to run */
    hmi_display_init();

    /* 4. Wait for display to finish booting (~500ms power-on time).
     *    Then drain any boot garbage from RX buffer before configuring.
     *    bkcmd=0 → no ACK responses (avoids RX flood).
     *    recmod=0 → passive mode: auto-send touch events on press. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    hmi_bsp_drain();
    hmi_display_send("bkcmd=0");
    hmi_display_send("recmod=0");
    vTaskDelay(pdMS_TO_TICKS(50));
    hmi_bsp_drain();  /* discard any ACK from bkcmd/recmod itself */
    ESP_LOGI(TAG, "Display init commands sent (bkcmd=0, recmod=0)");

    /* 5. Navigate display to home page BEFORE spawning the RX task.
     *    The RX task runs at a higher FreeRTOS priority than app_main (the
     *    caller).  If the TJC display sends startup/page-change bytes while
     *    goto_page is still running, the RX task preempts app_main on every
     *    vTaskDelay() call, starving it — hmi_task_enter_mode() never returns
     *    and pcf8563_init() is never reached.  Completing the page init first
     *    avoids that race entirely. */
    hmi_display_goto_page(HMI_PAGE_HOME);

    /* 6. Spawn the RX task — display is now in a known state */
    s_rx_stack = (StackType_t *)heap_caps_malloc(HMI_RX_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rx_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rx_stack || !s_rx_tcb) {
        ESP_LOGE(TAG, "Failed to allocate HMI RX task resources");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_active = true;
    s_rx_task = xTaskCreateStatic(rx_task_fn, "hmi_rx", HMI_RX_TASK_STACK, NULL,
                                  HMI_RX_TASK_PRIO, s_rx_stack, s_rx_tcb);
    if (!s_rx_task) {
        ESP_LOGE(TAG, "Failed to create HMI RX task");
        s_active = false;
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "HMI RX task created in PSRAM");

    hmi_task_unlock_display();

    for (int i = 0; i < 10 && !s_rx_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_rx_running) {
        ESP_LOGE(TAG, "HMI RX task did not start in time");
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
        s_active = false;
        hmi_task_free_rx_buffers();
        hmi_bsp_deinit();
        uart_switch_route_to_lan_mcu();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "HMI mode active");
    return ESP_OK;

fail:
    hmi_task_unlock_display();
    s_active = false;
    hmi_task_free_rx_buffers();
    hmi_bsp_deinit();
    uart_switch_route_to_lan_mcu();
    return ret;
}

esp_err_t hmi_task_exit_mode(void)
{
    if (!s_active) {
        ESP_LOGW(TAG, "HMI not active");
        return ESP_OK;
    }

    /* 1. Signal RX task to stop and wait (max 500 ms) */
    s_active = false;
    for (int i = 0; i < 25 && s_rx_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_rx_running) {
        ESP_LOGE(TAG, "RX task did not stop before timeout");
        return ESP_ERR_TIMEOUT;
    }
    s_rx_task = NULL;
    hmi_task_free_rx_buffers();

    /* 2. Deinit UART2 BSP driver */
    hmi_bsp_deinit();

    /* 3. Route UART2 switch back to LAN MCU path (GPIO46 LOW) */
    uart_switch_route_to_lan_mcu();

    ESP_LOGI(TAG, "HMI mode exited — UART2 returned to LAN MCU");
    return ESP_OK;
}

bool hmi_task_is_active(void)
{
    return s_active;
}

void hmi_task_update_status(const hmi_status_t *s)
{
    hmi_status_t snapshot;

    if (!s || !s_active) return;

    /* Update thread-safe cache */
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(&s_cached, s, sizeof(hmi_status_t));
        memcpy(&snapshot, &s_cached, sizeof(snapshot));
        xSemaphoreGive(s_status_mutex);
    } else {
        memcpy(&snapshot, s, sizeof(snapshot));
    }

    /* Push to the Middleware / display directly.
     * This is called from pwr_monitor_task at 5-second intervals — safe to
     * call from a task context since UART writes are non-blocking.        */
    if (hmi_task_lock_display(portMAX_DELAY)) {
        hmi_display_refresh_status(&snapshot);
        hmi_task_unlock_display();
    }
}
