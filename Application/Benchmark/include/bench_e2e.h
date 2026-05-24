/**
 * @file bench_e2e.h
 * @brief Internal end-to-end latency benchmark — WAN side gate.
 *
 * When enabled, two `[E2E_WAN]` log lines are emitted per uplink frame:
 *   stage=ingress  — from SPI RX inside process_data_from_lan to the moment
 *                    the payload is handed off to the server transport layer.
 *   stage=publish  — from MQTT queue enqueue to just before the actual
 *                    esp_mqtt_client_publish() call.
 *
 * Correlate the two stages by handler_type + temporal proximity. Total WAN
 * internal latency = ingress + (publish - enqueue) + actual publish call.
 *
 * Pair with `BENCH_E2E_LAN_ENABLE` on the LAN MCU (Section 2 Part A) for
 * the full internal E2E picture. SPI bridge cost itself is in Section 1.
 *
 * When disabled, all measurement code is compiled out — zero runtime cost.
 */

#ifndef BENCH_E2E_H
#define BENCH_E2E_H

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Master switch for WAN-side internal E2E latency logging.
 *   0 = OFF. Measurement code is fully compiled out.
 *   1 = ON.  Each forwarded frame emits two `[E2E_WAN]` lines.
 */
#define BENCH_E2E_WAN_ENABLE 1

#if BENCH_E2E_WAN_ENABLE
  /** Emit one `[E2E_WAN]` log line. Same args as `ESP_LOGI` body. */
  #define BENCH_E2E_WAN_LOG(fmt, ...) \
      ESP_LOGI("E2E_WAN", fmt, ##__VA_ARGS__)
#else
  #define BENCH_E2E_WAN_LOG(fmt, ...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BENCH_E2E_H */
