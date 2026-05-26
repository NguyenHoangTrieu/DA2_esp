#ifndef BENCH_LATENCY_WAN_H_
#define BENCH_LATENCY_WAN_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* §5 — End-to-end latency benchmark, WAN side.
 *
 * Receives HANDLER_LAT frames from LAN (each carries [T1 8B][seq 4B][payload]),
 * forwards payload to a raw TCP sink (the same Python tcp_sink.py used by §3),
 * stamps T2 right after a successful send(), and logs latency = T2 - T1.
 *
 * The active TCP card is whichever the gateway has up (WiFi / ETH / 4G).
 */

#ifndef BENCH_LATENCY_WAN_ENABLE
#define BENCH_LATENCY_WAN_ENABLE 0
#endif

/* Internal queue depth. 8 is enough at 200 pps with ~30 ms WiFi latency
 * (max in-flight ≈ 6). Larger values waste PSRAM. */
#ifndef BENCH_LATENCY_WAN_QUEUE_LEN
#define BENCH_LATENCY_WAN_QUEUE_LEN 8
#endif

/* Per-packet payload cap. RS485 spam from PC is 64 B by default — 256 covers
 * any realistic test. KEEP SMALL: queue storage = QUEUE_LEN × (12 + PKT_MAX). */
#ifndef BENCH_LATENCY_WAN_PKT_MAX
#define BENCH_LATENCY_WAN_PKT_MAX 256
#endif

/* Sink targets — picked at runtime based on the active card:
 *   - WiFi / Ethernet : your LAN PC running tcp_sink.py (reachable from LAN)
 *   - LTE (4G)        : a publicly reachable TCP echo (PC not reachable from
 *                       the cellular network, must go through a tunnel/echo)
 */
#ifndef BENCH_LATENCY_WAN_SINK_LAN_HOST
#define BENCH_LATENCY_WAN_SINK_LAN_HOST "192.168.1.100"
#endif
#ifndef BENCH_LATENCY_WAN_SINK_LAN_PORT
#define BENCH_LATENCY_WAN_SINK_LAN_PORT 5555
#endif
#ifndef BENCH_LATENCY_WAN_SINK_LTE_HOST
#define BENCH_LATENCY_WAN_SINK_LTE_HOST "bore.pub"
#endif
#ifndef BENCH_LATENCY_WAN_SINK_LTE_PORT
#define BENCH_LATENCY_WAN_SINK_LTE_PORT 43217
#endif

/* Reporting window: print per-window stats (min/median/p95/max/n/loss). */
#ifndef BENCH_LATENCY_WAN_REPORT_MS
#define BENCH_LATENCY_WAN_REPORT_MS 1000
#endif

esp_err_t bench_latency_wan_init(void);

/* Called from the LAN-handler frame dispatcher when handler_id == HANDLER_LAT.
 * payload points at [T1 8B][seq 4B][user_payload...]. */
void bench_latency_wan_on_frame(const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_LATENCY_WAN_H_ */
