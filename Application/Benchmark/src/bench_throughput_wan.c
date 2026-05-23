/**
 * @file bench_throughput_wan.c
 * @brief Inter-MCU SPI throughput benchmark — WAN side (SPI Slave), RX-only.
 *
 * Only measures the LAN→WAN direction:
 *   bench_throughput_wan_count_rx() is called by process_data_from_lan()
 *   for each arriving BNC frame sent by the LAN-side sender.
 *
 * Every BENCH_TP_WAN_REPORT_INTERVAL_MS a snapshot is printed:
 *
 *   [BENCH_TP_WAN 2000ms] RX(LAN->WAN): pkt=N b=N pps=X.X kbps=XXX.X
 *
 * Compile-time gate: BENCH_THROUGHPUT_WAN_ENABLE (bench_throughput_wan.h).
 *
 * NOTE: WAN->LAN sender removed — it was flooding mcu_lan_enqueue_downlink()
 *       causing persistent "Downlink queue full" and zero throughput.
 */

#include "bench_throughput_wan.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lan_comm.h"

/* For framing-stats reporting */
extern lan_comm_handle_t g_lan_handle;

static const char *TAG = "BENCH_TP_WAN";

#if BENCH_THROUGHPUT_WAN_ENABLE

/* ---------- Configuration ---------- */
#define BENCH_TP_WAN_TASK_STACK_WORDS (4096 / sizeof(StackType_t))
#define BENCH_TP_WAN_TASK_PRIORITY    2   /* below all real handler tasks */

/* ---------- Shared state (portMUX protected) ---------- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_rx_pkt = 0;
static volatile uint32_t s_rx_b   = 0;

static volatile bool s_running = false;

/* ---------- Public counter API ---------- */

void bench_throughput_wan_count_rx(uint32_t bytes) {
    portENTER_CRITICAL(&s_mux);
    s_rx_pkt++;
    s_rx_b += bytes;
    portEXIT_CRITICAL(&s_mux);
}

/* ---------- Reporter task ---------- */

static void bench_tp_wan_reporter_task(void *arg) {
    ESP_LOGI(TAG, "Reporter task started (interval=%d ms)",
             BENCH_TP_WAN_REPORT_INTERVAL_MS);

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(BENCH_TP_WAN_REPORT_INTERVAL_MS));
        if (!s_running) break;

        /* Atomic snapshot + reset */
        uint32_t rx_pkt, rx_b;

        portENTER_CRITICAL(&s_mux);
        rx_pkt = s_rx_pkt; s_rx_pkt = 0;
        rx_b   = s_rx_b;   s_rx_b   = 0;
        portEXIT_CRITICAL(&s_mux);

        const float interval_s = (float)BENCH_TP_WAN_REPORT_INTERVAL_MS / 1000.0f;
        const float rx_kbps = (interval_s > 0.0f)
            ? ((float)rx_b * 8.0f) / (interval_s * 1000.0f) : 0.0f;
        const float rx_pps  = (interval_s > 0.0f)
            ? (float)rx_pkt / interval_s : 0.0f;

        ESP_LOGI(TAG,
                 "[BENCH_TP_WAN %dms] RX(LAN->WAN): pkt=%lu b=%lu pps=%.1f kbps=%.1f",
                 BENCH_TP_WAN_REPORT_INTERVAL_MS,
                 (unsigned long)rx_pkt, (unsigned long)rx_b,
                 rx_pps, rx_kbps);

        /* P1 framing diagnostics — cumulative since boot. */
        if (g_lan_handle) {
            uint32_t fok = 0, hcrc = 0, pcrc = 0, resync = 0, gap = 0;
            lan_comm_get_framing_stats(g_lan_handle, &fok, &hcrc, &pcrc,
                                       &resync, &gap);
            ESP_LOGI(TAG,
                     "[BENCH_TP_WAN frame] rx_ok=%lu hdr_crc_fail=%lu "
                     "pay_crc_fail=%lu resync_bytes=%lu seq_gap=%lu",
                     (unsigned long)fok, (unsigned long)hcrc,
                     (unsigned long)pcrc, (unsigned long)resync,
                     (unsigned long)gap);
        }
    }

    ESP_LOGI(TAG, "Reporter task stopped");
    vTaskDelete(NULL);
}

/* ---------- Public lifecycle API ---------- */

esp_err_t bench_throughput_wan_start(void) {
    if (s_running) return ESP_OK;

    s_running = true;

    /* --- Reporter task only (no TX sender — WAN->LAN direction removed) --- */
    StackType_t  *rep_stack = (StackType_t *)heap_caps_malloc(
        BENCH_TP_WAN_TASK_STACK_WORDS * sizeof(StackType_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *rep_tcb   = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!rep_stack || !rep_tcb) {
        ESP_LOGE(TAG, "Failed to allocate reporter task memory");
        s_running = false;
        if (rep_stack) heap_caps_free(rep_stack);
        if (rep_tcb)   heap_caps_free(rep_tcb);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t h_reporter = xTaskCreateStatic(
        bench_tp_wan_reporter_task, "bench_tp_wan_rep",
        BENCH_TP_WAN_TASK_STACK_WORDS, NULL,
        BENCH_TP_WAN_TASK_PRIORITY,
        rep_stack, rep_tcb);

    if (!h_reporter) {
        ESP_LOGE(TAG, "Failed to create WAN reporter task");
        s_running = false;
        heap_caps_free(rep_stack);
        heap_caps_free(rep_tcb);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Inter-MCU WAN throughput benchmark started (RX-only, LAN->WAN)");
    ESP_LOGI(TAG, "  Report every : %d ms", BENCH_TP_WAN_REPORT_INTERVAL_MS);
    return ESP_OK;
}

void bench_throughput_wan_stop(void) {
    if (!s_running) return;
    s_running = false;
    ESP_LOGI(TAG, "Stop requested — tasks will self-delete");
}

#else /* BENCH_THROUGHPUT_WAN_ENABLE == 0 */

void bench_throughput_wan_count_rx(uint32_t bytes) { (void)bytes; }

esp_err_t bench_throughput_wan_start(void) {
    ESP_LOGI(TAG, "WAN throughput benchmark disabled (BENCH_THROUGHPUT_WAN_ENABLE=0)");
    return ESP_OK;
}

void bench_throughput_wan_stop(void) {}

#endif /* BENCH_THROUGHPUT_WAN_ENABLE */
