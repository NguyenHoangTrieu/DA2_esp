/**
 * @file bench_time_sync.c  (WAN MCU — slave responder)
 * @brief Counterpart to LAN-side bench_time_sync. The WAN MCU has no active
 *        task; it simply responds when the LAN master sends a TSYNC_REQ frame.
 *
 *        State (offset) is also tracked on this side from the same exchange:
 *        we know (T1, T4) only via what LAN tells us in subsequent frames, so
 *        the WAN-side offset converges via the embedded `lan_rx_us` on the
 *        next data frame (TS-4 integration). For now, only the slave response
 *        path is implemented here.
 */

#include "bench_time_sync.h"
#include "frame_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "TSYNC";

#if BENCH_TIME_SYNC_ENABLE

static SemaphoreHandle_t s_state_mutex = NULL;
static int64_t s_offset_us = 0;     /* lan_us − wan_us — inverse of LAN side */
static int64_t s_last_rtt_us = 0;
static uint32_t s_responded_count = 0;
static uint32_t s_responded_fail = 0;
static bool s_synced = false;

#endif /* BENCH_TIME_SYNC_ENABLE */

esp_err_t bench_time_sync_init(void) {
#if BENCH_TIME_SYNC_ENABLE
  if (s_state_mutex == NULL) {
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "slave responder ready");
#endif
  return ESP_OK;
}

int64_t bench_time_sync_to_peer_us(int64_t local_us) {
#if BENCH_TIME_SYNC_ENABLE
  int64_t off = 0;
  bool synced = false;
  if (s_state_mutex) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    off = s_offset_us;
    synced = s_synced;
    xSemaphoreGive(s_state_mutex);
  }
  /* From WAN's view: peer = LAN. peer_us = local_us + offset_us */
  return synced ? (local_us + off) : local_us;
#else
  return local_us;
#endif
}

int64_t bench_time_sync_from_peer_us(int64_t peer_us) {
#if BENCH_TIME_SYNC_ENABLE
  int64_t off = 0;
  bool synced = false;
  if (s_state_mutex) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    off = s_offset_us;
    synced = s_synced;
    xSemaphoreGive(s_state_mutex);
  }
  /* From WAN's view: peer_us is LAN clock → convert to WAN clock. */
  return synced ? (peer_us - off) : peer_us;
#else
  return peer_us;
#endif
}

void bench_time_sync_get_state(bench_time_sync_state_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
#if BENCH_TIME_SYNC_ENABLE
  if (s_state_mutex) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    out->offset_us = s_offset_us;
    out->last_rtt_us = s_last_rtt_us;
    out->median_rtt_us = 0; /* WAN side doesn't compute median */
    out->sync_count = s_responded_count;
    out->sync_fail = s_responded_fail;
    out->synced = s_synced;
    out->sync_age_ms = 0xFFFFFFFFu; /* WAN doesn't track timing of own responses */
    xSemaphoreGive(s_state_mutex);
  }
#endif
}

esp_err_t bench_time_sync_request_round(void) {
  /* Slave cannot initiate rounds. */
  return ESP_ERR_NOT_SUPPORTED;
}

bool bench_time_sync_slave_build_response(const uint8_t *req_payload,
                                          uint16_t req_len,
                                          int64_t now_t2_us,
                                          uint8_t *out_rsp,
                                          uint16_t out_cap,
                                          uint16_t *out_len) {
#if BENCH_TIME_SYNC_ENABLE
  if (!req_payload || !out_rsp || !out_len) return false;
  if (req_len < sizeof(tsync_request_t)) {
    ESP_LOGW(TAG, "Slave: req too short (%u)", (unsigned)req_len);
    if (s_state_mutex) {
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      s_responded_fail++;
      xSemaphoreGive(s_state_mutex);
    }
    return false;
  }
  if (out_cap < sizeof(tsync_response_t)) return false;

  const tsync_request_t *req = (const tsync_request_t *)req_payload;
  if (req->cmd != FRAME_TYPE_TSYNC_REQ) return false;

  /* Capture T3 as late as possible — right before we fill the reply buffer. */
  int64_t t3 = esp_timer_get_time();

  tsync_response_t rsp;
  rsp.ack_prefix = 0x02;
  rsp.ack_type   = ACK_TYPE_TSYNC_RSP;
  rsp.lan_t1_us  = req->lan_t1_us;
  rsp.wan_t2_us  = (uint64_t)now_t2_us;
  rsp.wan_t3_us  = (uint64_t)t3;
  memcpy(out_rsp, &rsp, sizeof(rsp));
  *out_len = sizeof(rsp);

  if (s_state_mutex) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_responded_count++;
    xSemaphoreGive(s_state_mutex);
  }
  ESP_LOGD(TAG, "Slave: resp t1=%llu t2=%lld t3=%lld",
           (unsigned long long)req->lan_t1_us, (long long)now_t2_us,
           (long long)t3);
  return true;
#else
  (void)req_payload; (void)req_len; (void)now_t2_us;
  (void)out_rsp; (void)out_cap; (void)out_len;
  return false;
#endif
}
