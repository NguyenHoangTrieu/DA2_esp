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
 *        1 = compile real sender + reporter tasks.
 *        0 = all functions compiled as no-ops (zero production overhead).
 */
#define BENCH_THROUGHPUT_WAN_ENABLE 0

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
