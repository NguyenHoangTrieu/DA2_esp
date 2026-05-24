/**
 * @file bench_throughput_wan.c
 * @brief Inter-MCU SPI throughput benchmark — WAN side (SPI Slave).
 *        LAN→WAN: count incoming BNC frames in process_data_from_lan.
 *        WAN→LAN: (Mode 1) static template / (Mode 2) refresh task.
 */

#include "bench_throughput_wan.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lan_comm.h"
#include "mcu_lan_handler.h"  /* Mode 2: mcu_lan_enqueue_downlink */
#include "frame_types.h"      /* HANDLER_BENCH */
#include <string.h>

/* For framing-stats reporting + TX template installation */
extern lan_comm_handle_t g_lan_handle;

static const char *TAG = "BENCH_TP_WAN";

#if BENCH_THROUGHPUT_WAN_ENABLE

/* WAN→LAN template. Inner: [DT][BNC][len BE][rtc 19B][payload 2048].
 * Loaded once on first BNC RX. Refresh during a run races with master
 * full-duplex DMA → CRC corruption; other slave paths must check
 * bench_throughput_wan_is_active() before overwriting tx_buffer. */
#define BENCH_WAN_TX_PAYLOAD_LEN  2048u
#define BENCH_WAN_TX_INNER_LEN    (2u + 3u + 2u + 19u + BENCH_WAN_TX_PAYLOAD_LEN) /* 2074 */

static uint8_t s_wan_tx_template[BENCH_WAN_TX_INNER_LEN];
static bool    s_wan_tx_template_built = false;
static volatile bool s_template_installed = false;

static void bench_wan_build_template_once(void) {
    if (s_wan_tx_template_built) return;
    s_wan_tx_template[0] = 'D';  s_wan_tx_template[1] = 'T';
    s_wan_tx_template[2] = 'B';  s_wan_tx_template[3] = 'N';  s_wan_tx_template[4] = 'C';
    uint16_t data_len = 19u + BENCH_WAN_TX_PAYLOAD_LEN;  /* 2067 */
    s_wan_tx_template[5] = (uint8_t)((data_len >> 8) & 0xFFu);
    s_wan_tx_template[6] = (uint8_t)(data_len & 0xFFu);
    memcpy(&s_wan_tx_template[7], "00/00/0000-00:00:00", 19);
    memset(&s_wan_tx_template[26], 0xAA, BENCH_WAN_TX_PAYLOAD_LEN);
    s_wan_tx_template_built = true;
}

/* Push template into slave tx_buffer. */
static void bench_wan_refresh_tx_template(void) {
    if (g_lan_handle == NULL) return;
    bench_wan_build_template_once();
    lan_comm_load_tx_data(g_lan_handle, s_wan_tx_template,
                          (uint16_t)BENCH_WAN_TX_INNER_LEN);
}

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

    /* Install template on first BNC RX (race-free — master has finished
     * handshake by the time any BNC arrives). One-shot, then static. */
    if (!s_template_installed) {
        s_template_installed = true;
        bench_wan_refresh_tx_template();
    }
}

bool bench_throughput_wan_is_active(void) {
    return s_running;
}

/* Mode 2 refresh task disabled — current lan_comm has no transaction-boundary
 * sync, so refreshing tx_buffer mid-DMA corrupts MISO. Restore when lan_comm
 * gains ping-pong tx_buffer or a "safe to refresh now" callback. */

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

    /* Mode 2 used to spawn a WAN→LAN refresh task here. That code is now
     * disabled (see the long commentary at the top of this file): in the
     * current lan_comm architecture there's no race-free way to refresh
     * tx_buffer mid-flight. Mode 2 LAN→WAN remains production-real via the
     * uplink dispatcher's ACK-gated branch; Mode 2 WAN→LAN reuses the
     * Mode 1 one-shot static template (installed by count_rx on first BNC). */

    ESP_LOGI(TAG,
             "Inter-MCU WAN throughput benchmark started (mode=%d: %s)",
             BENCH_THROUGHPUT_WAN_ENABLE,
#if BENCH_TP_WAN_MODE_DRIVER
             "DRIVER — static one-shot template after first BNC"
#elif BENCH_TP_WAN_MODE_PROD_REAL
             "PROD_REAL — LAN->WAN via ACK-gated dispatcher; WAN->LAN reverts to one-shot template"
#else
             "?"
#endif
             );
    ESP_LOGI(TAG, "  Report every : %d ms", BENCH_TP_WAN_REPORT_INTERVAL_MS);
    ESP_LOGI(TAG, "  WAN->LAN bench frame: %u bytes inner (static, no refresh)",
             (unsigned)BENCH_WAN_TX_INNER_LEN);
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

bool bench_throughput_wan_is_active(void) { return false; }

#endif /* BENCH_THROUGHPUT_WAN_ENABLE */
