/**
 * @file bench_throughput_wan.h
 * @brief Inter-MCU SPI throughput benchmark — WAN side (SPI Slave), RX-only.
 *
 * When BENCH_THROUGHPUT_WAN_ENABLE = 1 this module creates one reporter task:
 *
 *   bench_tp_wan_reporter — priority 2, prints RX summary every
 *                           BENCH_TP_WAN_REPORT_INTERVAL_MS via ESP_LOGI:
 *
 *     [BENCH_TP_WAN 2000ms] RX(LAN->WAN): pkt=N b=N pps=X.X kbps=XXX.X
 *
 * The LAN MCU (DA2_esp_LAN) runs bench_throughput which floods uplink BNC
 * frames (LAN→WAN direction). Each arriving BNC DT frame is counted here
 * via bench_throughput_wan_count_rx().
 *
 * NOTE: WAN→LAN sender removed — it was flooding mcu_lan_enqueue_downlink()
 *       causing persistent "Downlink queue full" and zero throughput.
 *
 * Master switch: set BENCH_THROUGHPUT_WAN_ENABLE to 1 to compile the real
 * implementation; 0 compiles all public functions as no-ops.
 */

#ifndef BENCH_THROUGHPUT_WAN_H
#define BENCH_THROUGHPUT_WAN_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Master on/off switch for the WAN-side inter-MCU throughput benchmark.
 *        Must be kept in sync with BENCH_THROUGHPUT_ENABLE on the LAN side.
 *
 *   0 = OFF. All public functions are no-ops.
 *
 *   1 = DRIVER mode. Slave loads ONE static [DT][BNC]...[0xAA*2048] template
 *       into tx_buffer on the first BNC RX (one-shot). Never refreshes.
 *       Master full-duplex pulls the same buffer every transaction. Measures
 *       transport ceiling, not real downlink generation cost.
 *
 *   2 = PRODUCTION-REAL mode. A refresh task periodically (every
 *       BENCH_TP_WAN_REFRESH_INTERVAL_MS) calls
 *       mcu_lan_enqueue_downlink(HANDLER_BENCH, ...) — the same path real
 *       downlink producers use (config push, RPC responses). Each refresh
 *       walks queue → memcpy → downlink_handler_task → lan_comm_load_tx_data,
 *       so the wire-time throughput reflects the cost of producing fresh
 *       payload, not echoing a static buffer.
 */
#define BENCH_THROUGHPUT_WAN_ENABLE 2

/* Derived flags — do NOT edit. */
#define BENCH_TP_WAN_MODE_OFF        (BENCH_THROUGHPUT_WAN_ENABLE == 0)
#define BENCH_TP_WAN_MODE_DRIVER     (BENCH_THROUGHPUT_WAN_ENABLE == 1)
#define BENCH_TP_WAN_MODE_PROD_REAL  (BENCH_THROUGHPUT_WAN_ENABLE == 2)

/** Refresh interval for Mode 2 downlink generator (ms).
 *
 *  Architectural note: real production downlinks (config push, RPC response,
 *  FOTA trigger) go through `send_downlink_to_lan` which is ACK-gated
 *  (3 retries × 500 ms ACK_WAIT_TIMEOUT_MS). Bench traffic, however, is
 *  short-circuited inside send_downlink_to_lan for HANDLER_BENCH:
 *  load_tx_data and return, no ACK wait. See the comment in
 *  mcu_lan_handler_uplink.c::send_downlink_to_lan for the rationale —
 *  short version: waiting on the application ACK on the uplink task
 *  starves master LAN→WAN traffic and produces a deadlock-ish cycle.
 *
 *  Consequence: Mode 2 WAN→LAN throughput is bounded by master full-duplex
 *  transaction rate, not by the ACK round-trip — so it tracks the SPI
 *  wire ceiling minus framing overhead.
 *
 *  Pacing rule of thumb at 2 KB payload:
 *      kbps ≈ 2048 × 8 / interval_ms  (capped by master pull rate)
 *      20 ms → ~820 kbps
 *      10 ms → ~1.6 Mbps
 *       5 ms → ~3.2 Mbps (master full-duplex ceiling on a 7-frame batch)
 *
 *  Don't push below 5 ms: lan_comm_load_tx_data writes tx_buffer mid-DMA
 *  when master is actively reading it, causing CRC corruption that the
 *  framing layer cannot recover (visible as resync_bytes spike). 20 ms is
 *  a safe default that exercises the full pipeline without racing. */
#define BENCH_TP_WAN_REFRESH_INTERVAL_MS 20

/** Reporting interval in milliseconds. */
#define BENCH_TP_WAN_REPORT_INTERVAL_MS 2000

/**
 * @brief Start the WAN-side reporter task.
 *        Call AFTER mcu_lan_handler_start().
 */
esp_err_t bench_throughput_wan_start(void);

/**
 * @brief Stop the reporter task.
 */
void bench_throughput_wan_stop(void);

/**
 * @brief Increment the RX byte counter (LAN→WAN direction).
 *        Called from process_data_from_lan() for HANDLER_BENCH frames.
 *
 * @param bytes Number of payload bytes in the received BNC frame.
 */
void bench_throughput_wan_count_rx(uint32_t bytes);

/**
 * @brief True when the WAN-side bench is running and the slave's tx_buffer
 *        holds the static WAN-to-LAN bench template. Other slave-side paths
 *        (RTC response, FOTA trigger, etc.) MUST NOT call lan_comm_load_tx_data
 *        while this returns true - overwriting the template both creates a
 *        DMA-vs-CPU race (master full-duplex reads tx_buffer in real time)
 *        and stops the WAN-to-LAN bench RX counter.
 */
bool bench_throughput_wan_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_THROUGHPUT_WAN_H */
