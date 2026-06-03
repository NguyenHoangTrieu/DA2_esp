#include "bench_latency_wan.h"

#if BENCH_LATENCY_WAN_ENABLE

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "mcu_lan_handler.h"
#include "config_handler.h"

extern config_internet_type_t g_internet_type;

static const char *TAG = "clk_lat_wan";

/* ─── Per-card endpoint for T2 ─────────────────────────────────────────────
 * WiFi:     T2 = when WiFi radio reports TX done (esp_wifi tx_done callback)
 * Ethernet: T2 = right after the netif linkoutput returns — for the W5500,
 *           that means the synchronous SPI write to the chip has finished
 * LTE:      T2 = right after the PPP netif linkoutput returns — for the
 *           SIM7600, that means the USB CDC write to the modem has finished
 */

typedef enum {
    LAT_CARD_NONE = 0,
    LAT_CARD_WIFI,
    LAT_CARD_ETH,
    LAT_CARD_LTE,
} lat_card_t;

static const char *card_label(void)
{
    switch (g_internet_type) {
        case CONFIG_INTERNET_WIFI:     return "wifi";
        case CONFIG_INTERNET_LTE:      return "4g";
        case CONFIG_INTERNET_ETHERNET: return "eth";
        default:                       return "?";
    }
}

static lat_card_t current_card(void)
{
    switch (g_internet_type) {
        case CONFIG_INTERNET_WIFI:     return LAT_CARD_WIFI;
        case CONFIG_INTERNET_ETHERNET: return LAT_CARD_ETH;
        case CONFIG_INTERNET_LTE:      return LAT_CARD_LTE;
        default:                       return LAT_CARD_NONE;
    }
}

static void pick_sink_target(const char **host, uint16_t *port)
{
    if (g_internet_type == CONFIG_INTERNET_LTE) {
        *host = BENCH_LATENCY_WAN_SINK_LTE_HOST;
        *port = BENCH_LATENCY_WAN_SINK_LTE_PORT;
    } else {
        *host = BENCH_LATENCY_WAN_SINK_LAN_HOST;
        *port = BENCH_LATENCY_WAN_SINK_LAN_PORT;
    }
}

/* Wire marker placed at the start of every TCP payload so the per-card
 * hook can recognise the bench packet inside the egress frame. */
#define LAT_MARK_MAGIC0  'B'
#define LAT_MARK_MAGIC1  'L'
#define LAT_MARK_MAGIC2  'A'
#define LAT_MARK_MAGIC3  'T'
#define LAT_MARK_BYTES   8   /* 4B magic + 4B seq LE */

typedef struct {
    uint64_t t1_us;
    uint32_t seq;
    uint8_t  source_id;
    uint16_t payload_len;
    uint8_t  payload[BENCH_LATENCY_WAN_PKT_MAX];
} lat_item_t;

static QueueHandle_t s_q = NULL;
static StaticQueue_t s_q_cb;
static uint8_t      *s_q_storage = NULL;

/* Per-window stats */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t n;
    uint64_t lat_min_us;
    uint64_t lat_max_us;
    uint64_t lat_sum_us;
    uint32_t loss;
    uint32_t qdrop;       /* packets dropped because the latency queue was full */
    uint32_t send_fail;
    uint32_t tx_timeout;
    uint64_t send_us_sum; /* cumulative send() call duration (to locate the bottleneck) */
} s_w = {.lat_min_us = UINT64_MAX};

/* Latency histogram for percentiles (p50/p95/p99). Linear 1 ms bins; the
 * sender bumps one bin per delivered packet (cheap), the reporter snapshots
 * and resets once per window. Samples beyond the range fall in the last bin's
 * tail and are covered by reporting max alongside the percentiles. */
#define LAT_HIST_BINS   512          /* 0 .. 511 ms */
#define LAT_HIST_BIN_US 1000         /* 1 ms per bin */
static uint32_t s_hist[LAT_HIST_BINS];
static uint32_t s_hist_ovf;          /* >= LAT_HIST_BINS ms */
static uint32_t s_hist_snap[LAT_HIST_BINS]; /* reporter-only scratch (avoids 2KB stack) */

static uint32_t s_last_seq = 0;
static bool     s_have_last_seq = false;

/* ─── Pending-packet slot (single in-flight; sender_task is serial) ────── */
static portMUX_TYPE       s_pend_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_pend_armed = false;
static volatile uint32_t  s_pend_seq   = 0;
static volatile uint64_t  s_pend_t2_us = 0;
static SemaphoreHandle_t  s_tx_done_sem = NULL;

/* ─── Hook state ───────────────────────────────────────────────────────── */
static struct netif *s_hooked_netif    = NULL;     /* eth or ppp */
static netif_linkoutput_fn s_orig_linkoutput = NULL;
static lat_card_t s_hook_card = LAT_CARD_NONE;

/* Scan a flat buffer for "BLAT" + seq (LE). */
static inline bool buf_has_marker(const uint8_t *buf, size_t len, uint32_t seq)
{
    if (!buf || len < LAT_MARK_BYTES) return false;
    const uint8_t s0 = (uint8_t)(seq);
    const uint8_t s1 = (uint8_t)(seq >> 8);
    const uint8_t s2 = (uint8_t)(seq >> 16);
    const uint8_t s3 = (uint8_t)(seq >> 24);
    for (size_t i = 0; i + LAT_MARK_BYTES <= len; i++) {
        if (buf[i]   == LAT_MARK_MAGIC0 &&
            buf[i+1] == LAT_MARK_MAGIC1 &&
            buf[i+2] == LAT_MARK_MAGIC2 &&
            buf[i+3] == LAT_MARK_MAGIC3 &&
            buf[i+4] == s0 && buf[i+5] == s1 &&
            buf[i+6] == s2 && buf[i+7] == s3) {
            return true;
        }
    }
    return false;
}

/* Called by any per-card hook when our pending packet has been observed
 * leaving the card. Records T2 and wakes the sender_task. */
static inline void mark_tx_observed(uint64_t t2_us)
{
    portENTER_CRITICAL(&s_pend_mux);
    bool armed = s_pend_armed;
    if (armed) {
        s_pend_armed = false;
        s_pend_t2_us = t2_us;
    }
    portEXIT_CRITICAL(&s_pend_mux);
    if (armed && s_tx_done_sem) {
        xSemaphoreGive(s_tx_done_sem);
    }
}

/* ─── WiFi: use send() returns (not internal tx_done API) ──────────────── */
/* WiFi TX completion detected via linkoutput (async, less precise than Eth/LTE).
 * WiFi linkoutput is not blocking; packet goes to driver queue.
 * For simplicity and stability, WiFi T2 = after send() returns (TCP buffer accept).
 * The async WiFi MAC layer behavior is still captured because send() blocks
 * when TCP window is full, which happens when WiFi can't drain fast enough. */

/* ─── Ethernet / LTE linkoutput wrapper ─────────────────────────────────── */
static err_t bench_linkoutput_wrap(struct netif *nif, struct pbuf *p)
{
    err_t r = s_orig_linkoutput(nif, p);
    /* For W5500: linkoutput synchronously pushes the frame over SPI.
     * For PPP (USB CDC): linkoutput pushes the PPP frame over USB CDC.
     * Either way, returning means the bytes have been handed to the
     * peripheral. */
    if (r != ERR_OK) return r;

    uint32_t seq;
    bool armed;
    portENTER_CRITICAL(&s_pend_mux);
    armed = s_pend_armed;
    seq   = s_pend_seq;
    portEXIT_CRITICAL(&s_pend_mux);
    if (!armed) return r;

    for (struct pbuf *q = p; q; q = q->next) {
        if (buf_has_marker((const uint8_t *)q->payload, q->len, seq)) {
            mark_tx_observed((uint64_t)esp_timer_get_time());
            break;
        }
    }
    return r;
}

/* Find a netif by lwIP-style name prefix (e.g. "en" for eth, "pp" for PPP). */
static struct netif *find_lwip_netif(char c0, char c1)
{
    struct netif *nif;
    for (nif = netif_list; nif != NULL; nif = nif->next) {
        if (nif->name[0] == c0 && nif->name[1] == c1) return nif;
    }
    return NULL;
}

/* Install the linkoutput hook for Ethernet only.
 * WiFi and LTE use send() returns directly (see sender_task):
 *  - WiFi: no public TX-done API.
 *  - LTE/PPP: PPP doesn't use the standard netif->linkoutput; its output
 *    flows through pppos_output_callback → HDLC encoder → USB CDC. Hooking
 *    that path correctly is fragile and IDF-version-dependent. */
static void install_card_hook(void)
{
    lat_card_t card = current_card();

    /* WiFi and LTE don't use the linkoutput hook. */
    if (card == LAT_CARD_WIFI || card == LAT_CARD_LTE) return;

    /* If hook is already installed for this card, nothing to do. */
    if (s_hooked_netif && s_hook_card == card) return;

    if (card == LAT_CARD_ETH) {
        /* eth uses lwIP name "en" / "e0". */
        struct netif *nif = find_lwip_netif('e', 'n');
        if (!nif) {
            /* Fallback: scan for any netif whose name is not WiFi/PPP. */
            for (struct netif *t = netif_list; t; t = t->next) {
                if (t->name[0] != 's' && t->name[0] != 'a' && t->name[0] != 'p') {
                    nif = t; break;
                }
            }
        }
        if (!nif || !nif->linkoutput) {
            ESP_LOGW(TAG, "linkoutput hook: no candidate netif yet");
            return;
        }
        if (nif->linkoutput == bench_linkoutput_wrap) return;  /* already */
        s_orig_linkoutput  = nif->linkoutput;
        nif->linkoutput    = bench_linkoutput_wrap;
        s_hooked_netif     = nif;
        s_hook_card        = card;
        ESP_LOGI(TAG, "linkoutput hook installed on netif '%c%c%d' (card=%s)",
                 nif->name[0], nif->name[1], (int)nif->num, card_label());
    }
}

static int open_sink(void)
{
    const char *host = NULL;
    uint16_t    port = 0;
    pick_sink_target(&host, &port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() errno=%d", errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct addrinfo hints = {0};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        char port_s[8];
        snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
        int rc = getaddrinfo(host, port_s, &hints, &res);
        if (rc != 0 || !res) {
            ESP_LOGE(TAG, "getaddrinfo(%s:%u) failed rc=%d", host, port, rc);
            if (res) freeaddrinfo(res);
            close(fd);
            return -1;
        }
        memcpy(&addr, res->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(res);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "connect %s:%u errno=%d", host, port, errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "TCP sink connected %s:%u (card=%s)", host, port, card_label());
    return fd;
}

static void sender_task(void *arg)
{
    (void)arg;
    int fd = -1;
    lat_item_t *item = (lat_item_t *)heap_caps_malloc(sizeof(lat_item_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Outgoing buffer = 8B marker + payload */
    uint8_t *out = (uint8_t *)heap_caps_malloc(LAT_MARK_BYTES + BENCH_LATENCY_WAN_PKT_MAX,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!item || !out) {
        ESP_LOGE(TAG, "alloc sender buffers failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_q, item, portMAX_DELAY) != pdTRUE) continue;

        /* Bench latency bypasses the internet_status gate — we want to
         * keep trying to push packets out the active card even if the
         * gateway considers internet "offline" (e.g., LTE link up but
         * the public sink isn't reachable). T2 is captured at the
         * card boundary, not at the server. */
        if (fd < 0) {
            fd = open_sink();
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            /* Just (re)connected. Drop whatever piled up while the sink was
             * unreachable — those are startup-stale packets whose huge wait
             * time would otherwise seed a permanent standing backlog and
             * corrupt the steady-state latency. Start measuring fresh. */
            xQueueReset(s_q);
        }

        /* Install the per-card hook lazily — netif/Wi-Fi must be up. */
        install_card_hook();

        /* Latency benchmarks must NOT run under WiFi power-save / light-sleep:
         * modem-sleep + automatic light-sleep defer each TX to a wake/beacon
         * slot, inflating per-send time and capping the egress packet rate
         * (→ standing-queue latency at high load). Force the radio fully awake
         * once, when WiFi is the active card. One-shot; harmless if repeated. */
        static bool s_ps_off = false;
        if (!s_ps_off && current_card() == LAT_CARD_WIFI) {
            if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
                s_ps_off = true;
                ESP_LOGI(TAG, "WiFi power-save DISABLED for latency bench (WIFI_PS_NONE)");
            }
        }

        /* Build payload: 4B magic + 4B seq LE + user payload. */
        out[0] = LAT_MARK_MAGIC0;
        out[1] = LAT_MARK_MAGIC1;
        out[2] = LAT_MARK_MAGIC2;
        out[3] = LAT_MARK_MAGIC3;
        out[4] = (uint8_t)(item->seq);
        out[5] = (uint8_t)(item->seq >> 8);
        out[6] = (uint8_t)(item->seq >> 16);
        out[7] = (uint8_t)(item->seq >> 24);
        memcpy(&out[LAT_MARK_BYTES], item->payload, item->payload_len);
        uint16_t out_len = LAT_MARK_BYTES + item->payload_len;

        /* Arm pending slot BEFORE send() so a fast hook can win the race. */
        xSemaphoreTake(s_tx_done_sem, 0);   /* drain stale */
        portENTER_CRITICAL(&s_pend_mux);
        s_pend_seq    = item->seq;
        s_pend_t2_us  = 0;
        s_pend_armed  = true;
        portEXIT_CRITICAL(&s_pend_mux);

        int64_t t_s0 = esp_timer_get_time();
        int n = send(fd, out, out_len, 0);
        int64_t send_dur_us = esp_timer_get_time() - t_s0;
        if (n <= 0) {
            portENTER_CRITICAL(&s_pend_mux); s_pend_armed = false; portEXIT_CRITICAL(&s_pend_mux);
            portENTER_CRITICAL(&s_mux); s_w.send_fail++; portEXIT_CRITICAL(&s_mux);
            ESP_LOGW(TAG, "send() errno=%d, reopening", errno);
            close(fd); fd = -1;
            continue;
        }

        /* Capture T2 based on active card:
         * - WiFi: T2 = right after send() returns (TCP buffer accept).
         * - LTE:  T2 = right after send() returns (PPP linkoutput unhookable).
         * - Eth:  T2 = when linkoutput wrap fires (SPI write to W5500 done). */
        uint64_t t2_us = 0;
        lat_card_t card_now = current_card();
        if (card_now == LAT_CARD_WIFI || card_now == LAT_CARD_LTE) {
            /* WiFi / LTE: T2 set immediately after send(). */
            t2_us = (uint64_t)esp_timer_get_time();
            portENTER_CRITICAL(&s_pend_mux); s_pend_armed = false; portEXIT_CRITICAL(&s_pend_mux);
        } else {
            /* Ethernet / LTE: wait for hook to mark T2. Cap at 1 s to avoid hanging. */
            bool got = (xSemaphoreTake(s_tx_done_sem, pdMS_TO_TICKS(1000)) == pdTRUE);
            if (got) {
                portENTER_CRITICAL(&s_pend_mux);
                t2_us = s_pend_t2_us;
                portEXIT_CRITICAL(&s_pend_mux);
            } else {
                portENTER_CRITICAL(&s_pend_mux); s_pend_armed = false; portEXIT_CRITICAL(&s_pend_mux);
                portENTER_CRITICAL(&s_mux); s_w.tx_timeout++; portEXIT_CRITICAL(&s_mux);
                ESP_LOGW(TAG, "TX-done hook timeout (seq=%lu, card=%s)",
                         (unsigned long)item->seq, card_label());
                continue;
            }
        }

        uint64_t lat_us = (t2_us > item->t1_us) ? (t2_us - item->t1_us) : 0;

        uint32_t bin = (uint32_t)(lat_us / LAT_HIST_BIN_US);
        portENTER_CRITICAL(&s_mux);
        s_w.n++;
        s_w.lat_sum_us += lat_us;
        if (lat_us < s_w.lat_min_us) s_w.lat_min_us = lat_us;
        if (lat_us > s_w.lat_max_us) s_w.lat_max_us = lat_us;
        s_w.send_us_sum += (uint64_t)send_dur_us;
        if (bin < LAT_HIST_BINS) s_hist[bin]++; else s_hist_ovf++;
        portEXIT_CRITICAL(&s_mux);

        /* Per-packet logging is OFF by default — it blocks the sender on the
         * console UART and caps throughput. The WIN line carries the
         * distribution instead. Enable only for low-rate debugging. */
#if BENCH_LATENCY_WAN_LOG_EACH
        ESP_LOGI(TAG, "LAT card=%s seq=%lu src=0x%02X t1=%llu t2=%llu lat_us=%llu lat_ms=%.3f",
                 card_label(),
                 (unsigned long)item->seq,
                 (unsigned)item->source_id,
                 (unsigned long long)item->t1_us,
                 (unsigned long long)t2_us,
                 (unsigned long long)lat_us,
                 (double)lat_us / 1000.0);
#endif
    }
}

/* Percentile (ms) from the 1 ms histogram. Denominator n = all delivered
 * samples (finite bins + overflow). If the target rank falls in the >range
 * tail, return max_ms (the captured maximum) rather than a wrong small value. */
static double pct_ms(const uint32_t *h, uint32_t n, int p, double max_ms)
{
    if (n == 0) return 0.0;
    uint64_t target = ((uint64_t)n * (uint64_t)p + 99) / 100; /* ceil */
    uint64_t cum = 0;
    for (int i = 0; i < LAT_HIST_BINS; i++) {
        cum += h[i];
        if (cum >= target)
            return ((double)i + 1.0) * ((double)LAT_HIST_BIN_US / 1000.0);
    }
    return max_ms;
}

/* Mean of only the samples at or below cutoff_ms — i.e. with the periodic RTC
 * spike (≈ p50 + 150 ms) trimmed off. 1 ms bins, bin i ≈ (i + 0.5) ms. */
static double trimmed_avg_ms(const uint32_t *h, double cutoff_ms)
{
    int cut_bin = (int)cutoff_ms;
    if (cut_bin < 0) cut_bin = 0;
    if (cut_bin >= LAT_HIST_BINS) cut_bin = LAT_HIST_BINS - 1;
    double   wsum = 0.0;
    uint64_t cnt  = 0;
    for (int i = 0; i <= cut_bin; i++) {
        cnt  += h[i];
        wsum += (double)h[i] * ((double)i + 0.5);
    }
    return cnt ? (wsum / (double)cnt) : 0.0;
}

static void reporter_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BENCH_LATENCY_WAN_REPORT_MS));
        uint32_t n, loss, qdrop, fail, tmo, ovf;
        uint64_t mn, mx, sum, send_sum;
        portENTER_CRITICAL(&s_mux);
        n = s_w.n; loss = s_w.loss; qdrop = s_w.qdrop;
        fail = s_w.send_fail; tmo = s_w.tx_timeout;
        mn = s_w.lat_min_us; mx = s_w.lat_max_us; sum = s_w.lat_sum_us;
        send_sum = s_w.send_us_sum;
        ovf = s_hist_ovf;
        memcpy(s_hist_snap, s_hist, sizeof(s_hist));
        s_w.n = 0; s_w.loss = 0; s_w.qdrop = 0; s_w.send_fail = 0; s_w.tx_timeout = 0;
        s_w.lat_sum_us = 0; s_w.send_us_sum = 0;
        s_w.lat_min_us = UINT64_MAX; s_w.lat_max_us = 0;
        memset(s_hist, 0, sizeof(s_hist));
        s_hist_ovf = 0;
        portEXIT_CRITICAL(&s_mux);

        /* Standing depth of the WAN latency queue right now — tells us whether
         * the backlog sits here (WAN-bound) or upstream (LAN queue / SPI). */
        uint32_t qdepth = s_q ? (uint32_t)uxQueueMessagesWaiting(s_q) : 0;

        if (n == 0) {
            ESP_LOGI(TAG, "WIN card=%s n=0 (idle) qdrop=%lu loss=%lu",
                     card_label(), (unsigned long)qdrop, (unsigned long)loss);
        } else {
            double mn_ms  = mn == UINT64_MAX ? 0.0 : (double)mn / 1000.0;
            double mx_ms  = (double)mx / 1000.0;
            double avg_ms = (double)(sum / n) / 1000.0;
            double p50 = pct_ms(s_hist_snap, n, 50, mx_ms);
            double p95 = pct_ms(s_hist_snap, n, 95, mx_ms);
            double p99 = pct_ms(s_hist_snap, n, 99, mx_ms);
            double send_ms = (double)(send_sum / n) / 1000.0; /* avg send() duration */
            /* Trimmed avg: steady latency with the periodic RTC spike removed. */
            double avgX = trimmed_avg_ms(s_hist_snap, p50 + BENCH_LATENCY_WAN_TRIM_MS);
            ESP_LOGI(TAG,
                     "WIN card=%s n=%lu min=%.3f avg=%.3f avgX=%.1f p50=%.0f p95=%.0f p99=%.0f max=%.3f ms "
                     "send=%.2f qd=%lu | loss=%lu qdrop=%lu ovf=%lu send_fail=%lu tx_tmo=%lu",
                     card_label(), (unsigned long)n,
                     mn_ms, avg_ms, avgX, p50, p95, p99, mx_ms,
                     send_ms, (unsigned long)qdepth,
                     (unsigned long)loss, (unsigned long)qdrop, (unsigned long)ovf,
                     (unsigned long)fail, (unsigned long)tmo);
        }
    }
}

esp_err_t bench_latency_wan_init(void)
{
    if (s_q) return ESP_OK;

    const size_t storage_bytes =
        (size_t)BENCH_LATENCY_WAN_QUEUE_LEN * sizeof(lat_item_t);
    s_q_storage = (uint8_t *)heap_caps_malloc(
        storage_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_q_storage) {
        ESP_LOGE(TAG, "queue storage alloc failed (%u bytes PSRAM)",
                 (unsigned)storage_bytes);
        return ESP_ERR_NO_MEM;
    }
    s_q = xQueueCreateStatic(BENCH_LATENCY_WAN_QUEUE_LEN,
                             sizeof(lat_item_t),
                             s_q_storage, &s_q_cb);
    if (!s_q) {
        heap_caps_free(s_q_storage);
        s_q_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_tx_done_sem = xSemaphoreCreateBinary();
    if (!s_tx_done_sem) {
        ESP_LOGE(TAG, "tx_done semaphore create failed");
        return ESP_ERR_NO_MEM;
    }

    {
        const size_t stack_w = (6 * 1024) / sizeof(StackType_t);
        StackType_t  *stk = heap_caps_malloc(stack_w * sizeof(StackType_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!stk || !tcb) return ESP_ERR_NO_MEM;
        if (!xTaskCreateStatic(sender_task, "bench_lat_tx",
                               stack_w, NULL, 5, stk, tcb)) return ESP_FAIL;
    }
    {
        const size_t stack_w = (3 * 1024) / sizeof(StackType_t);
        StackType_t  *stk = heap_caps_malloc(stack_w * sizeof(StackType_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!stk || !tcb) return ESP_ERR_NO_MEM;
        if (!xTaskCreateStatic(reporter_task, "bench_lat_rp",
                               stack_w, NULL, 3, stk, tcb)) return ESP_FAIL;
    }

    ESP_LOGI(TAG, "bench latency WAN ready (per-card T2: wifi=tx_done, eth=spi_done, lte=usb_done)");
    return ESP_OK;
}

void bench_latency_wan_on_frame(const uint8_t *payload, uint16_t len)
{
    if (!s_q || !payload || len < 12) return;

    lat_item_t item;
    item.t1_us = 0;
    for (int i = 0; i < 8; i++) item.t1_us |= ((uint64_t)payload[i]) << (8 * i);
    item.seq = ((uint32_t)payload[8])        |
               ((uint32_t)payload[9]  << 8)  |
               ((uint32_t)payload[10] << 16) |
               ((uint32_t)payload[11] << 24);
    item.source_id = 0;

    uint16_t plen = len - 12;
    if (plen > BENCH_LATENCY_WAN_PKT_MAX) plen = BENCH_LATENCY_WAN_PKT_MAX;
    memcpy(item.payload, payload + 12, plen);
    item.payload_len = plen;

    portENTER_CRITICAL(&s_mux);
    if (s_have_last_seq) {
        uint32_t expected = s_last_seq + 1;
        if (item.seq > expected) s_w.loss += (item.seq - expected);
    } else {
        s_have_last_seq = true;
    }
    s_last_seq = item.seq;
    portEXIT_CRITICAL(&s_mux);

    if (xQueueSend(s_q, &item, 0) != pdTRUE) {
        /* Count, don't log: on_frame runs in the LAN-RX dispatch context and a
         * per-drop log line would stall the SPI bridge at high load (and flood
         * the console). The WIN line reports qdrop instead. */
        portENTER_CRITICAL(&s_mux);
        s_w.qdrop++;
        portEXIT_CRITICAL(&s_mux);
    }
}

#else  /* BENCH_LATENCY_WAN_ENABLE */

esp_err_t bench_latency_wan_init(void) { return ESP_OK; }
void bench_latency_wan_on_frame(const uint8_t *p, uint16_t l) { (void)p; (void)l; }

#endif /* BENCH_LATENCY_WAN_ENABLE */
