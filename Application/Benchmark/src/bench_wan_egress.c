/**
 * @file bench_wan_egress.c
 * @brief Raw TCP egress benchmark — measures per-card upload rate against
 *        a self-hosted LAN sink (bypasses MQTT/broker entirely).
 */

#include "bench_wan_egress.h"

#if BENCH_WAN_EGRESS_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "config_handler.h"
#include "mcu_lan_handler.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "bench_egr";

#define BENCH_TASK_STACK_WORDS (4096 / sizeof(StackType_t))

extern config_internet_type_t g_internet_type;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_gen_pkt        = 0;
static volatile uint32_t s_gen_bytes      = 0;
static volatile uint32_t s_sent_ok_pkt    = 0;
static volatile uint32_t s_sent_ok_bytes  = 0;
static volatile uint32_t s_sent_fail      = 0;
static volatile uint32_t s_q_drop         = 0;

static volatile bool s_running = false;

static inline void inc_gen(uint32_t bytes) {
    portENTER_CRITICAL(&s_mux);
    s_gen_pkt++;
    s_gen_bytes += bytes;
    portEXIT_CRITICAL(&s_mux);
}

static inline void inc_sent_ok(uint32_t bytes) {
    portENTER_CRITICAL(&s_mux);
    s_sent_ok_pkt++;
    s_sent_ok_bytes += bytes;
    portEXIT_CRITICAL(&s_mux);
}

static inline void inc_sent_fail(void) {
    portENTER_CRITICAL(&s_mux);
    s_sent_fail++;
    portEXIT_CRITICAL(&s_mux);
}

static inline void inc_q_drop(void) {
    portENTER_CRITICAL(&s_mux);
    s_q_drop++;
    portEXIT_CRITICAL(&s_mux);
}

static const char *card_label(void) {
#if BENCH_WAN_EGRESS_CARD == 1
    const char *compile_label = "wifi";
#elif BENCH_WAN_EGRESS_CARD == 2
    const char *compile_label = "eth";
#elif BENCH_WAN_EGRESS_CARD == 3
    const char *compile_label = "4g";
#else
    const char *compile_label = "?";
#endif
    switch (g_internet_type) {
        case CONFIG_INTERNET_WIFI:     return "wifi";
        case CONFIG_INTERNET_LTE:      return "4g";
        case CONFIG_INTERNET_ETHERNET: return "eth";
        default:                       return compile_label;
    }
}

/* Pick sink host/port based on the active card. LTE goes to tcpbin.com
 * (public TCP echo) so 4G testing doesn't need a hosted sink. */
static void pick_sink_target(const char **host, uint16_t *port) {
    if (g_internet_type == CONFIG_INTERNET_LTE) {
        *host = BENCH_WAN_EGRESS_SINK_LTE_HOST;
        *port = BENCH_WAN_EGRESS_SINK_LTE_PORT;
    } else {
        /* WiFi, Ethernet, or unknown — assume operator runs tcp_sink.py on LAN */
        *host = BENCH_WAN_EGRESS_SINK_LAN_HOST;
        *port = BENCH_WAN_EGRESS_SINK_LAN_PORT;
    }
}

static int open_tcp_sink(void) {
    const char *host = NULL;
    uint16_t    port = 0;
    pick_sink_target(&host, &port);

    /* Resolve via DNS so tcpbin.com works without hard-coded IP. */
    struct addrinfo hints = { 0 };
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGE(TAG, "getaddrinfo(%s:%u) failed rc=%d", host, port, rc);
        if (res) freeaddrinfo(res);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "connect() to %s:%u failed errno=%d", host, port, errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    /* Non-blocking: send() returns EAGAIN/EWOULDBLOCK when lwIP TX buf full —
     * that's our saturation signal (q_drop). Blocking mode would just rate-limit
     * the producer silently and hide the ceiling. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "TCP sink connected: %s:%u (non-blocking, card=%s)",
             host, port, card_label());
    return fd;
}

static void bench_wan_egress_producer_task(void *arg) {
    (void)arg;
    static uint8_t fake_buf[BENCH_WAN_EGRESS_PKT_SIZE];
    for (size_t i = 0; i < sizeof(fake_buf); i++) {
        fake_buf[i] = (uint8_t)(i & 0xFF);
    }

    int fd = -1;
    ESP_LOGI(TAG, "Raw TCP producer started (size=%d)",
             BENCH_WAN_EGRESS_PKT_SIZE);

    while (s_running) {
        /* Guard against using socket/netif during LTE/WiFi/ETH teardown.
         * Without this check, the producer keeps calling connect()/send()
         * while the network stack is being destroyed (e.g. when
         * LTE_HANDLER restarts a zombie modem), which races with the
         * driver shutdown and can panic on a null netif. */
        if (mcu_lan_handler_get_internet_status() != INTERNET_STATUS_ONLINE) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (fd < 0) {
            fd = open_tcp_sink();
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        inc_gen((uint32_t)sizeof(fake_buf));
        int n = send(fd, fake_buf, sizeof(fake_buf), 0);
        if (n > 0) {
            inc_sent_ok((uint32_t)n);
            /* Partial send: remaining bytes count as q_drop (we don't retry
             * the tail — bench cares about ceiling, not integrity). */
            if ((size_t)n < sizeof(fake_buf)) {
                inc_q_drop();
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* lwIP TX buffer full ⇒ card is saturated. This is the signal we
             * want — don't reopen, just yield briefly and try again. */
            inc_q_drop();
            taskYIELD();
        } else {
            inc_sent_fail();
            ESP_LOGW(TAG, "send() failed errno=%d, reopening", errno);
            close(fd);
            fd = -1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (fd >= 0) close(fd);
    ESP_LOGI(TAG, "Raw TCP producer stopped");
    vTaskDelete(NULL);
}

static void bench_wan_egress_reporter_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Egress reporter started (card=%s interval=%d ms)",
             card_label(), BENCH_WAN_EGRESS_REPORT_INTERVAL_MS);

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(BENCH_WAN_EGRESS_REPORT_INTERVAL_MS));
        if (!s_running) break;

        uint32_t gen_pkt, gen_b, ok_pkt, ok_b, fail, qdrop;

        portENTER_CRITICAL(&s_mux);
        gen_pkt = s_gen_pkt;   gen_b = s_gen_bytes;
        ok_pkt  = s_sent_ok_pkt; ok_b = s_sent_ok_bytes;
        fail    = s_sent_fail;
        qdrop   = s_q_drop;
        s_gen_pkt = s_gen_bytes = 0;
        s_sent_ok_pkt = s_sent_ok_bytes = 0;
        s_sent_fail = 0;
        s_q_drop = 0;
        portEXIT_CRITICAL(&s_mux);

        const float interval_s =
            (float)BENCH_WAN_EGRESS_REPORT_INTERVAL_MS / 1000.0f;
        const float gen_kbps =
            interval_s > 0.0f
                ? ((float)gen_b * 8.0f) / (interval_s * 1000.0f)
                : 0.0f;
        const float ok_kbps =
            interval_s > 0.0f
                ? ((float)ok_b * 8.0f) / (interval_s * 1000.0f)
                : 0.0f;
        const float ok_pps =
            interval_s > 0.0f ? (float)ok_pkt / interval_s : 0.0f;

        ESP_LOGI(TAG,
                 "WAN_EGRESS card=%s gen_pkt=%lu gen_b=%lu "
                 "sent_ok_pkt=%lu sent_ok_b=%lu sent_fail=%lu q_drop=%lu "
                 "ok_pps=%.1f gen_kbps=%.1f ok_kbps=%.1f",
                 card_label(),
                 (unsigned long)gen_pkt, (unsigned long)gen_b,
                 (unsigned long)ok_pkt, (unsigned long)ok_b,
                 (unsigned long)fail, (unsigned long)qdrop,
                 ok_pps, gen_kbps, ok_kbps);
    }

    ESP_LOGI(TAG, "Egress reporter stopped");
    vTaskDelete(NULL);
}

esp_err_t bench_wan_egress_start(void) {
    if (s_running) return ESP_OK;
    s_running = true;

    /* Reporter task */
    {
        StackType_t  *stack = (StackType_t *)heap_caps_malloc(
            BENCH_TASK_STACK_WORDS * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!stack || !tcb) {
            if (stack) heap_caps_free(stack);
            if (tcb)   heap_caps_free(tcb);
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
        TaskHandle_t h = xTaskCreateStatic(
            bench_wan_egress_reporter_task, "bench_egr_r",
            BENCH_TASK_STACK_WORDS, NULL, 3, stack, tcb);
        if (!h) {
            heap_caps_free(stack);
            heap_caps_free(tcb);
            s_running = false;
            return ESP_FAIL;
        }
    }

    /* Producer task */
    {
        StackType_t  *stack = (StackType_t *)heap_caps_malloc(
            BENCH_TASK_STACK_WORDS * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!stack || !tcb) {
            if (stack) heap_caps_free(stack);
            if (tcb)   heap_caps_free(tcb);
            ESP_LOGE(TAG, "Producer alloc failed (reporter still up)");
            return ESP_ERR_NO_MEM;
        }
        TaskHandle_t h = xTaskCreateStatic(
            bench_wan_egress_producer_task, "bench_egr_p",
            BENCH_TASK_STACK_WORDS, NULL, 4, stack, tcb);
        if (!h) {
            heap_caps_free(stack);
            heap_caps_free(tcb);
            ESP_LOGE(TAG, "Producer task create failed");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "WAN egress bench started");
    return ESP_OK;
}

void bench_wan_egress_stop(void) {
    s_running = false;
}

#endif /* BENCH_WAN_EGRESS_ENABLE */
