/**
 * @file bench_throughput_wan.h
 * @brief Inter-MCU SPI throughput benchmark — WAN side (SPI Slave).
 *        Reporter task prints RX summary every BENCH_TP_WAN_REPORT_INTERVAL_MS.
 *        LAN MCU drives the LAN→WAN flood via mcu_wan_try_enqueue_uplink.
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
 * @brief Master on/off + mode switch (keep in sync with LAN side).
 *   0 = OFF.
 *   1 = DRIVER. Slave loads ONE static template into tx_buffer on first BNC
 *       RX, never refreshes. Master pulls the same buffer full-duplex.
 *       Measures the transport ceiling.
 *   2 = PRODUCTION-REAL. A refresh task periodically calls
 *       mcu_lan_enqueue_downlink(HANDLER_BENCH, ...) so each refresh walks
 *       queue → memcpy → downlink_handler_task → lan_comm_load_tx_data.
 *       Measures real downlink generation cost.
 */
#define BENCH_THROUGHPUT_WAN_ENABLE 0

#define BENCH_TP_WAN_MODE_OFF        (BENCH_THROUGHPUT_WAN_ENABLE == 0)
#define BENCH_TP_WAN_MODE_DRIVER     (BENCH_THROUGHPUT_WAN_ENABLE == 1)
#define BENCH_TP_WAN_MODE_PROD_REAL  (BENCH_THROUGHPUT_WAN_ENABLE == 2)

/** Mode 2 refresh interval (ms). Don't drop below ~5 ms or DMA races appear. */
#define BENCH_TP_WAN_REFRESH_INTERVAL_MS 20

/** Reporter interval (ms). */
#define BENCH_TP_WAN_REPORT_INTERVAL_MS 2000

/** Start reporter task. Call after mcu_lan_handler_start(). */
esp_err_t bench_throughput_wan_start(void);

/** Stop reporter task. */
void bench_throughput_wan_stop(void);

/** Increment RX counter (LAN→WAN), called from process_data_from_lan. */
void bench_throughput_wan_count_rx(uint32_t bytes);

/**
 * @brief True when the WAN-side bench is running and the slave's tx_buffer
 *        holds the static template. Other slave paths (RTC response,
 *        FOTA, etc.) MUST NOT call lan_comm_load_tx_data while this is
 *        true — it would clobber the template and create a DMA-vs-CPU race.
 */
bool bench_throughput_wan_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_THROUGHPUT_WAN_H */
