/**
 * @file bench_wan_egress.h
 * @brief WAN-side egress benchmark — measures max upload rate per WAN card
 *        via a raw TCP socket to a LAN sink server.
 *
 * Why raw TCP instead of MQTT: the production MQTT path goes through a cloud
 * broker (ThingsBoard demo) that throttles or bans hosts spamming traffic, so
 * MQTT throughput would be limited by the broker, not the card. Raw TCP to a
 * self-hosted LAN sink reveals the true card ceiling.
 *
 * Counters:
 *   gen        : producer attempted to send a fake packet
 *   sent_ok    : socket send() accepted the payload (full or partial)
 *   sent_fail  : socket send() returned a hard error (TCP reset / disconnect)
 *   q_drop     : lwIP TX buffer full (EAGAIN/EWOULDBLOCK) ⇒ card saturated
 *                — non-zero q_drop is the canonical saturation signal.
 *
 * Card under test: physically/cfg leave only ONE card up (WiFi / ETH / 4G).
 * BENCH_WAN_EGRESS_CARD is only a label for the log line; the actual route is
 * whichever interface esp_netif promotes to default.
 */

#ifndef BENCH_WAN_EGRESS_H
#define BENCH_WAN_EGRESS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Master switch.
 *   0 = OFF (no counters, no producer, zero overhead).
 *   1 = ON  (raw TCP producer + counter + reporter).
 */
#define BENCH_WAN_EGRESS_ENABLE 0

/**
 * @brief Sink targets — picked at runtime based on the active card:
 *   - WiFi / Ethernet : your LAN PC running tcp_sink.py
 *   - LTE (4G)        : tcpbin.com TCP echo (public, no host needed)
 *
 * For LTE the firmware self-report (sent_ok / q_drop) is what we trust;
 * tcpbin echoes RX traffic but the firmware never reads it, so RX just
 * sits in lwIP RX buffer until TCP window closes — back-pressure will
 * eventually saturate via the wrong direction, biasing the ceiling a bit
 * low. Good enough for "what does this card sustain" smoke measurement.
 */
#define BENCH_WAN_EGRESS_SINK_LAN_HOST  "192.168.1.100"
#define BENCH_WAN_EGRESS_SINK_LAN_PORT  5555
#define BENCH_WAN_EGRESS_SINK_LTE_HOST  "bore.pub"
#define BENCH_WAN_EGRESS_SINK_LTE_PORT  54628

/**
 * @brief Compile-time label for the WAN card under test.
 *   1 = WiFi, 2 = Ethernet, 3 = 4G/LTE.
 * Only affects log labelling; the actual card is whichever is active.
 */
#define BENCH_WAN_EGRESS_CARD 1

/** Producer packet size (bytes). 1024 is a good balance vs MCU socket buf. */
#define BENCH_WAN_EGRESS_PKT_SIZE 1024

/** Reporter interval. */
#define BENCH_WAN_EGRESS_REPORT_INTERVAL_MS 2000

#if BENCH_WAN_EGRESS_ENABLE

/** Start producer + reporter. Call after the chosen WAN card is up. */
esp_err_t bench_wan_egress_start(void);

/** Stop both tasks. */
void bench_wan_egress_stop(void);

#else /* OFF — stubs */

static inline esp_err_t bench_wan_egress_start(void) { return ESP_OK; }
static inline void bench_wan_egress_stop(void) {}

#endif /* BENCH_WAN_EGRESS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* BENCH_WAN_EGRESS_H */
