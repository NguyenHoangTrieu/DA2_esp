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
static SemaphoreHandle_t s_mutex       = NULL;
static hmi_status_t      s_cached      = {0};

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
    ESP_LOGI(TAG, "RX task started");
    s_rx_running = true;

    uint8_t frame[256];
    while (s_active) {
        int len = hmi_bsp_read_frame(frame, sizeof(frame), 200);
        if (len > 0) {
            hmi_display_handle_frame(frame, len);
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

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    hmi_display_init();
    ESP_LOGI(TAG, "HMI task initialised (inactive)");
}

esp_err_t hmi_task_enter_mode(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "Already in HMI mode");
        return ESP_OK;
    }

    /* 1. Route UART2 switch to HMI display path (GPIO46 HIGH) */
    uart_switch_route_to_hmi();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Install UART2 BSP driver */
    esp_err_t ret = hmi_bsp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %s", esp_err_to_name(ret));
        uart_switch_route_to_lan_mcu();
        return ret;
    }

    /* 3. Mark active and reset Middleware state */
    s_active = true;
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
    {
        StackType_t *rx_stack = (StackType_t *)heap_caps_malloc(HMI_RX_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *rx_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (rx_stack && rx_tcb) {
            s_rx_task = xTaskCreateStatic(rx_task_fn, "hmi_rx", HMI_RX_TASK_STACK, NULL, HMI_RX_TASK_PRIO, rx_stack, rx_tcb);
            ESP_LOGI(TAG, "HMI RX task created in PSRAM");
        } else {
            ESP_LOGE(TAG, "Failed to allocate HMI RX task in PSRAM");
        }
    }

    ESP_LOGI(TAG, "HMI mode active");
    return ESP_OK;
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
    s_rx_task = NULL;

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
    if (!s || !s_active) return;

    /* Update thread-safe cache */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(&s_cached, s, sizeof(hmi_status_t));
        xSemaphoreGive(s_mutex);
    }

    /* Push to the Middleware / display directly.
     * This is called from pwr_monitor_task at 5-second intervals — safe to
     * call from a task context since UART writes are non-blocking.        */
    hmi_display_refresh_status(s);
}
