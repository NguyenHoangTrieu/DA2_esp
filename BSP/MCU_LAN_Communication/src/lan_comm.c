#include "lan_comm.h"
#include "spi_framing.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "LAN_COMM_SLAVE";

struct lan_comm_handle_s {
  lan_comm_config_t config;

  /* P3.a: ping-pong RX slots. While master clocks slot N, slot N+1 is already
   * queued in the SPI driver — eliminates the inter-frame gap that previously
   * dropped ~5% of master frames to slave-FIFO overflow. */
  spi_slave_transaction_t spi_trans[LAN_COMM_RX_QUEUE_DEPTH];
  uint8_t                *rx_buffer[LAN_COMM_RX_QUEUE_DEPTH];
  bool                    slot_queued[LAN_COMM_RX_QUEUE_DEPTH];

  uint8_t *tx_buffer;            /* shared across all slots */
  size_t   tx_buffer_len;
  SemaphoreHandle_t buffer_mutex;

  bool is_initialized;
  bool gpio_configured;
  lan_comm_status_t last_error;

  uint32_t packets_received;
  uint32_t packets_sent;
  uint32_t error_count;

  /* P1 framing */
  uint8_t  tx_seq;
  uint16_t rx_prev_seq;
  spi_frame_stats_t frame_stats;

  /* P3.b cumulative ACK: highest master seq we've successfully parsed.
   * SPI_FRAME_ACK_NONE means "haven't received anything yet — don't ack".
   * Every outgoing frame piggybacks this. */
  uint16_t last_rx_seq_for_ack;

  /* P3.c: stateful multi-frame drain. When master batches N frames into a
   * single SPI transaction, the slave's get_received_packet() iterates
   * through them by remembering position within the just-completed slot.
   * draining_slot = -1 means "no in-progress slot, pull the next done one";
   * else it's the slot index whose rx_buffer is still being walked.        */
  int    draining_slot;
  size_t drain_offset;
  size_t drain_remaining;
};

static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size,
                                              spi_frame_stats_t *stats,
                                              uint16_t *prev_seq);
static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error, const char *context);
static esp_err_t setup_data_ready_gpio(int gpio_pin);

lan_comm_status_t lan_comm_init(const lan_comm_config_t *config,
                                lan_comm_handle_t *handle) {
  if (config == NULL || handle == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "============================================");
  ESP_LOGI(TAG, "SPI Slave Initialization (Full-Duplex)");
  ESP_LOGI(TAG, "============================================");

  lan_comm_handle_t h =
      (lan_comm_handle_t)calloc(1, sizeof(struct lan_comm_handle_s));
  if (h == NULL) {
    ESP_LOGE(TAG, "Failed to allocate handle");
    return LAN_COMM_ERR_NOMEM;
  }

  memcpy(&h->config, config, sizeof(lan_comm_config_t));

  if (h->config.rx_buffer_size == 0) {
    h->config.rx_buffer_size = LAN_COMM_DEFAULT_RX_BUFFER;
  }
  if (h->config.tx_buffer_size == 0) {
    h->config.tx_buffer_size = LAN_COMM_DEFAULT_TX_BUFFER;
  }
  if (h->config.dma_channel == 0) {
    h->config.dma_channel = SPI_DMA_CH_AUTO;
  }

  bool alloc_ok = true;
  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    h->rx_buffer[i] =
        (uint8_t *)heap_caps_malloc(h->config.rx_buffer_size, MALLOC_CAP_DMA);
    if (h->rx_buffer[i] == NULL) {
      alloc_ok = false;
    }
  }
  h->tx_buffer =
      (uint8_t *)heap_caps_malloc(h->config.tx_buffer_size, MALLOC_CAP_DMA);
  h->buffer_mutex = xSemaphoreCreateMutex();
  h->tx_buffer_len = 0;

  if (!alloc_ok || h->tx_buffer == NULL || h->buffer_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to allocate buffers or mutex");
    for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
      if (h->rx_buffer[i])
        heap_caps_free(h->rx_buffer[i]);
    }
    if (h->tx_buffer)
      heap_caps_free(h->tx_buffer);
    if (h->buffer_mutex)
      vSemaphoreDelete(h->buffer_mutex);
    free(h);
    return LAN_COMM_ERR_NOMEM;
  }

  /* P3.a: only zero TX (handed to master as MISO when slave has no payload).
   * RX buffers are not zeroed per-cycle — parser is CRC + SOF-hunt based and
   * only inspects trans_len/8 bytes, so stale tail beyond that is invisible. */
  memset(h->tx_buffer, 0, h->config.tx_buffer_size);

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = config->gpio_io0,
      .miso_io_num = config->gpio_io1,
      .sclk_io_num = config->gpio_sck,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = h->config.rx_buffer_size,
      .flags = 0};

  spi_slave_interface_config_t slave_cfg = {.spics_io_num = config->gpio_cs,
                                            .flags = 0,
                                            .queue_size =
                                                LAN_COMM_TRANS_QUEUE_SIZE,
                                            .mode = config->mode,
                                            .post_setup_cb = NULL,
                                            .post_trans_cb = NULL};

  esp_err_t ret = spi_slave_initialize(config->host_id, &bus_cfg, &slave_cfg,
                                       h->config.dma_channel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI slave: %s", esp_err_to_name(ret));
    for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
      if (h->rx_buffer[i])
        heap_caps_free(h->rx_buffer[i]);
    }
    heap_caps_free(h->tx_buffer);
    vSemaphoreDelete(h->buffer_mutex);
    free(h);
    return LAN_COMM_ERR_INVALID_STATE;
  }

  gpio_set_pull_mode(config->gpio_cs, GPIO_PULLUP_ONLY);

  h->gpio_configured = false;
  if (config->gpio_data_ready >= 0) {
    if (setup_data_ready_gpio(config->gpio_data_ready) == ESP_OK) {
      h->gpio_configured = true;
      ESP_LOGI(TAG, "Data-Ready GPIO: GPIO%d", config->gpio_data_ready);
    } else {
      ESP_LOGW(TAG, "Failed to setup GPIO%d, data-ready disabled",
               config->gpio_data_ready);
    }
  }

  h->is_initialized = true;
  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    h->slot_queued[i] = false;
  }
  h->last_error = LAN_COMM_OK;
  h->packets_received = 0;
  h->packets_sent = 0;
  h->error_count = 0;
  h->tx_seq = 0;
  h->rx_prev_seq = 0xFFFFu;
  h->last_rx_seq_for_ack = SPI_FRAME_ACK_NONE;
  h->draining_slot = -1;
  h->drain_offset = 0;
  h->drain_remaining = 0;
  memset(&h->frame_stats, 0, sizeof(h->frame_stats));

  *handle = h;

  ESP_LOGI(TAG, "SPI slave ready (full-duplex, RX depth=%d)",
           LAN_COMM_RX_QUEUE_DEPTH);
  ESP_LOGI(TAG, "RX buffer: %u bytes × %d slots, TX buffer: %u bytes",
           (unsigned)h->config.rx_buffer_size, LAN_COMM_RX_QUEUE_DEPTH,
           (unsigned)h->config.tx_buffer_size);

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Deinitializing SPI slave");

  if (handle->gpio_configured && handle->config.gpio_data_ready >= 0) {
    gpio_reset_pin(handle->config.gpio_data_ready);
  }

  spi_slave_free(handle->config.host_id);

  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    if (handle->rx_buffer[i])
      heap_caps_free(handle->rx_buffer[i]);
  }
  heap_caps_free(handle->tx_buffer);
  vSemaphoreDelete(handle->buffer_mutex);

  handle->is_initialized = false;
  free(handle);

  return LAN_COMM_OK;
}

/* P3.a: queue a single free slot. Caller already holds buffer_mutex. */
static lan_comm_status_t queue_slot_locked(lan_comm_handle_t h, int idx) {
  if (h->slot_queued[idx]) {
    return LAN_COMM_OK;
  }
  spi_slave_transaction_t *t = &h->spi_trans[idx];
  memset(t, 0, sizeof(*t));
  t->length    = h->config.rx_buffer_size * 8;
  t->rx_buffer = h->rx_buffer[idx];
  t->tx_buffer = h->tx_buffer;        /* shared TX content across all slots */

  esp_err_t ret = spi_slave_queue_trans(h->config.host_id, t, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to queue slot %d: %s", idx, esp_err_to_name(ret));
    return LAN_COMM_ERR_BUS_BUSY;
  }
  h->slot_queued[idx] = true;
  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "queue_receive mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  /* Fill every free slot. First call after init queues all
   * LAN_COMM_RX_QUEUE_DEPTH slots; subsequent calls top up the slot the
   * caller just freed in get_received_packet.
   *
   * P3.c: skip the slot currently being drained — its rx_buffer still holds
   * unparsed frames from the master's batch, and re-queueing it would have
   * the SPI driver overwrite them with new master bytes.                    */
  lan_comm_status_t status = LAN_COMM_OK;
  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    if (i == handle->draining_slot) {
      continue;
    }
    lan_comm_status_t s = queue_slot_locked(handle, i);
    if (s != LAN_COMM_OK) {
      status = s;
    }
  }

  xSemaphoreGive(handle->buffer_mutex);

  if (status != LAN_COMM_OK) {
    lan_comm_report_error(handle, status, "queue_receive failed");
  }
  return status;
}

/* P3.c: try to parse one more frame out of the slot currently being drained.
 * Returns LAN_COMM_OK with packet filled, LAN_COMM_ERR_NO_DATA if exhausted,
 * or another status on parse error. On exhaustion the draining_slot is
 * cleared so the caller falls through to pull the next transaction.        */
static lan_comm_status_t drain_next_frame(lan_comm_handle_t handle,
                                          lan_comm_packet_t *packet) {
  if (handle->draining_slot < 0 || handle->drain_remaining < SPI_FRAME_OVERHEAD) {
    handle->draining_slot = -1;
    return LAN_COMM_ERR_NO_DATA;
  }

  size_t frame_size = 0;
  const uint8_t *buf =
      handle->rx_buffer[handle->draining_slot] + handle->drain_offset;
  lan_comm_status_t status =
      lan_comm_parse_frame(buf, handle->drain_remaining, packet, &frame_size,
                           &handle->frame_stats, &handle->rx_prev_seq);

  if (status == LAN_COMM_OK && frame_size > 0) {
    handle->drain_offset    += frame_size;
    handle->drain_remaining -= frame_size;
    handle->last_rx_seq_for_ack = (uint16_t)(handle->rx_prev_seq & 0xFFu);
    handle->packets_received++;
    return LAN_COMM_OK;
  }

  /* No more (or unrecoverable) frames in this slot — drop it and signal
   * "pull next transaction".                                                 */
  handle->draining_slot   = -1;
  handle->drain_offset    = 0;
  handle->drain_remaining = 0;
  return LAN_COMM_ERR_NO_DATA;
}

lan_comm_status_t lan_comm_get_received_packet(lan_comm_handle_t handle,
                                               lan_comm_packet_t *packet,
                                               uint32_t timeout_ms) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (packet == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  /* P3.c: first try to drain the slot that has unparsed frames left over
   * from the previous call. Master batches up to WAN_COMM_BATCH_MAX_FRAMES
   * into one SPI transaction, so a single slot may yield several packets. */
  if (handle->draining_slot >= 0) {
    lan_comm_status_t s = drain_next_frame(handle, packet);
    if (s == LAN_COMM_OK) {
      return LAN_COMM_OK;
    }
    /* drained — fall through to pull next transaction */
  }

  /* At least one slot must be queued. queue_receive() should have ensured
   * that — soft-fail loudly if not. */
  bool any_queued = false;
  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    if (handle->slot_queued[i]) { any_queued = true; break; }
  }
  if (!any_queued) {
    ESP_LOGW(TAG, "No transaction queued, call lan_comm_queue_receive() first");
    return LAN_COMM_ERR_INVALID_STATE;
  }

  spi_slave_transaction_t *trans;
  TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  esp_err_t ret =
      spi_slave_get_trans_result(handle->config.host_id, &trans, ticks);

  if (ret == ESP_ERR_TIMEOUT) {
    return LAN_COMM_ERR_TIMEOUT;
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get transaction result: %s", esp_err_to_name(ret));
    /* Mark all slots free so the next queue_receive can recover. */
    for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
      handle->slot_queued[i] = false;
    }
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "get_received_packet failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  /* Identify which slot completed by pointer match. */
  int idx = -1;
  for (int i = 0; i < LAN_COMM_RX_QUEUE_DEPTH; i++) {
    if (trans == &handle->spi_trans[i]) { idx = i; break; }
  }
  if (idx < 0) {
    ESP_LOGE(TAG, "Got unknown transaction pointer from driver");
    lan_comm_report_error(handle, LAN_COMM_ERR_INVALID_STATE,
                          "unknown trans pointer");
    return LAN_COMM_ERR_INVALID_STATE;
  }
  handle->slot_queued[idx] = false;

  if (trans->trans_len == 0) {
    return LAN_COMM_ERR_NO_DATA;
  }

  /* P3.c: stash drain state and let drain_next_frame() handle the first
   * (and any subsequent) frame from this slot. */
  handle->draining_slot   = idx;
  handle->drain_offset    = 0;
  handle->drain_remaining = trans->trans_len / 8;

  return drain_next_frame(handle, packet);
}

uint32_t lan_comm_parse_dma_buffer(lan_comm_handle_t handle,
                                   const uint8_t *buffer, size_t length,
                                   lan_comm_frame_callback_t callback,
                                   void *user_arg) {
  if (!handle || !buffer || length == 0 || !callback) {
    return 0;
  }

  uint32_t frame_count = 0;
  size_t offset = 0;

  while (offset < length) {
    lan_comm_packet_t packet;
    size_t frame_size = 0;
    lan_comm_status_t status =
        lan_comm_parse_frame(&buffer[offset], length - offset, &packet,
                             &frame_size, &handle->frame_stats,
                             &handle->rx_prev_seq);
    if (status == LAN_COMM_OK && frame_size > 0) {
      handle->last_rx_seq_for_ack = (uint16_t)(handle->rx_prev_seq & 0xFFu);
      callback(&packet, user_arg);
      frame_count++;
      offset += frame_size;
    } else if (frame_size > 0) {
      offset += frame_size;
    } else {
      /* TRUNCATED / NO_SYNC at tail — nothing more usable */
      break;
    }
  }

  return frame_count;
}

lan_comm_status_t lan_comm_load_tx_data(lan_comm_handle_t handle,
                                        const uint8_t *data_to_send,
                                        uint16_t length) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (data_to_send == NULL || length == 0) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  /* The framed length is SPI_FRAME_OVERHEAD + length; reject if it does not
   * fit. Also clamp the *inner* payload to the framing maximum. */
  if ((size_t)length > SPI_FRAME_MAX_PAYLOAD) {
    ESP_LOGE(TAG, "TX inner length %u exceeds frame max %u", length,
             (unsigned)SPI_FRAME_MAX_PAYLOAD);
    return LAN_COMM_ERR_INVALID_ARG;
  }
  size_t framed_len = SPI_FRAME_OVERHEAD + (size_t)length;
  if (framed_len > handle->config.tx_buffer_size) {
    ESP_LOGE(TAG, "TX framed length %u exceeds buffer size %u",
             (unsigned)framed_len, (unsigned)handle->config.tx_buffer_size);
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "load_tx_data mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  /* Zero only the bytes that were previously loaded (stale tail) plus the new
   * framed payload — avoids clearing the full 16 KB buffer every ACK load.   */
  size_t clear_len = (handle->tx_buffer_len > framed_len)
                         ? handle->tx_buffer_len
                         : framed_len;
  memset(handle->tx_buffer, 0, clear_len);

  /* P3.b: piggyback cumulative ACK = highest master seq we've parsed OK. */
  size_t built = spi_frame_build(handle->tx_buffer,
                                  handle->config.tx_buffer_size,
                                  SPI_FT_USER_BLOB, handle->tx_seq++,
                                  handle->last_rx_seq_for_ack,
                                  data_to_send, length);
  if (built == 0) {
    xSemaphoreGive(handle->buffer_mutex);
    lan_comm_report_error(handle, LAN_COMM_ERR_INVALID_ARG,
                          "load_tx_data frame build failed");
    return LAN_COMM_ERR_INVALID_ARG;
  }
  handle->tx_buffer_len = built;

  xSemaphoreGive(handle->buffer_mutex);

  handle->packets_sent++;

  if (handle->gpio_configured && handle->config.auto_signal_data_ready) {
    lan_comm_signal_data_ready(handle);
  }

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_signal_data_ready(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (!handle->gpio_configured || handle->config.gpio_data_ready < 0) {
    return LAN_COMM_ERR_INVALID_STATE;
  }

  gpio_set_level(handle->config.gpio_data_ready, 0);
  ets_delay_us(LAN_COMM_GPIO_PULSE_US);
  gpio_set_level(handle->config.gpio_data_ready, 1);

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle) {
  if (handle == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }
  return handle->last_error;
}

lan_comm_status_t lan_comm_get_statistics(lan_comm_handle_t handle,
                                          uint32_t *packets_received,
                                          uint32_t *errors) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (packets_received)
    *packets_received = handle->packets_received;
  if (errors)
    *errors = handle->error_count;

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_clear_statistics(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  handle->packets_received = 0;
  handle->packets_sent = 0;
  handle->error_count = 0;

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_get_framing_stats(lan_comm_handle_t handle,
                                              uint32_t *rx_frames_ok,
                                              uint32_t *rx_hdr_crc_fail,
                                              uint32_t *rx_payload_crc_fail,
                                              uint32_t *rx_resync_bytes,
                                              uint32_t *rx_seq_gap) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }
  if (rx_frames_ok)        *rx_frames_ok        = handle->frame_stats.frames_ok;
  if (rx_hdr_crc_fail)     *rx_hdr_crc_fail     = handle->frame_stats.hdr_crc_fail;
  if (rx_payload_crc_fail) *rx_payload_crc_fail = handle->frame_stats.payload_crc_fail;
  if (rx_resync_bytes)     *rx_resync_bytes     = handle->frame_stats.resync_bytes;
  if (rx_seq_gap)          *rx_seq_gap          = handle->frame_stats.seq_gap;
  return LAN_COMM_OK;
}

/* P1: parse one SPI frame out of the raw RX buffer. The frame's inner
 * payload still begins with the legacy 2-byte CF/DT/DQ/CQ magic so existing
 * dispatch logic in the uplink/downlink handlers works unchanged. */
static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size,
                                              spi_frame_stats_t *stats,
                                              uint16_t *prev_seq) {
  if (buffer == NULL || length == 0 || packet == NULL || frame_size == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  spi_frame_view_t view;
  spi_frame_status_t st = SPI_FRAME_NO_SYNC;
  size_t consumed = 0;
  bool ok = spi_frame_find(buffer, length, &view, &st, stats, &consumed);

  if (!ok) {
    if (st == SPI_FRAME_TRUNCATED || st == SPI_FRAME_NO_SYNC) {
      return LAN_COMM_ERR_NO_DATA;
    }
    /* BAD_HDR_CRC, BAD_PAYLOAD_CRC, BAD_LEN — counters already bumped */
    return LAN_COMM_ERR_INVALID_HEADER;
  }

  if (view.len < LAN_COMM_HEADER_SIZE) {
    return LAN_COMM_ERR_NO_DATA;
  }

  uint16_t inner_hdr = ((uint16_t)view.payload[0] << 8) | (uint16_t)view.payload[1];
  if (inner_hdr != LAN_COMM_HEADER_CF && inner_hdr != LAN_COMM_HEADER_DT &&
      inner_hdr != LAN_COMM_HEADER_DQ && inner_hdr != LAN_COMM_HEADER_CQ) {
    return LAN_COMM_ERR_INVALID_HEADER;
  }

  if (prev_seq) {
    spi_frame_track_seq(prev_seq, view.seq, stats);
  }

  packet->header_type = inner_hdr;
  packet->payload = (uint8_t *)view.payload;   /* still inside rx_buffer */
  packet->payload_length = view.len;
  *frame_size = consumed;
  return LAN_COMM_OK;
}

static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error,
                                  const char *context) {
  if (handle == NULL) {
    return;
  }
  handle->last_error = error;
  handle->error_count++;
  ESP_LOGE(TAG, "Error #%lu (code=%d): %s", handle->error_count, error,
           context);
}

static esp_err_t setup_data_ready_gpio(int gpio_pin) {
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << gpio_pin),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure GPIO%d: %s", gpio_pin,
             esp_err_to_name(ret));
    return ret;
  }

  gpio_set_level(gpio_pin, 0);
  return ESP_OK;
}
