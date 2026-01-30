#include "lan_comm.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "LAN_COMM_SLAVE";

/**
 * @brief Internal handle structure
 */
struct lan_comm_handle_s {
  // Configuration
  lan_comm_config_t config;
  // DMA-aligned buffers (fixed 4KB per Section 15.2)
  uint8_t *rx_buffer;
  uint8_t *tx_buffer;
  size_t rx_buffer_size_aligned;
  size_t tx_buffer_size_aligned;
  // Transaction descriptors
  spi_slave_hd_data_t rx_trans;
  spi_slave_hd_data_t tx_trans;
  // Synchronization
  SemaphoreHandle_t rx_mutex;
  SemaphoreHandle_t tx_mutex;
  // State
  bool is_initialized;
  bool rx_queued;
  bool tx_loaded;
  bool gpio_configured;
  lan_comm_status_t last_error;
  // Statistics
  uint32_t packets_received;
  uint32_t packets_sent;
  uint32_t frames_parsed;         // Total frames extracted from DMA buffers
  uint32_t padding_bytes_skipped; // Total 0x00 padding skipped
  uint32_t error_count;
};

static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size);
static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error, const char *context);
static bool is_dma_aligned(const void *ptr, size_t size);
static esp_err_t setup_data_ready_gpio(int gpio_pin);
static size_t calculate_dma_descriptors(size_t buffer_size);

#define CLEANUP_INIT(handle)                                                   \
  do {                                                                         \
    if (handle) {                                                              \
      if (handle->rx_mutex)                                                    \
        vSemaphoreDelete(handle->rx_mutex);                                    \
      if (handle->tx_mutex)                                                    \
        vSemaphoreDelete(handle->tx_mutex);                                    \
      if (handle->rx_buffer)                                                   \
        heap_caps_free(handle->rx_buffer);                                     \
      if (handle->tx_buffer)                                                   \
        heap_caps_free(handle->tx_buffer);                                     \
      if (handle->is_initialized)                                              \
        spi_slave_hd_deinit(handle->config.host_id);                           \
      free(handle);                                                            \
    }                                                                          \
  } while (0)

lan_comm_status_t lan_comm_init(const lan_comm_config_t *config,
                                lan_comm_handle_t *handle) {
  if (!config || !handle) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "============================================");
  ESP_LOGI(TAG, "QSPI Slave HD Initialization (WAN MCU)");
  ESP_LOGI(TAG, "============================================");

  // Validate GPIO pins
  if (config->gpio_sck < 0 || config->gpio_cs < 0 || config->gpio_io0 < 0 ||
      config->gpio_io1 < 0) {
    ESP_LOGE(TAG, "Invalid GPIO configuration");
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (config->enable_quad_mode &&
      (config->gpio_io2 < 0 || config->gpio_io3 < 0)) {
    ESP_LOGE(TAG, "QSPI mode requires IO2 and IO3 pins");
    return LAN_COMM_ERR_INVALID_ARG;
  }

  // Allocate handle
  lan_comm_handle_t h =
      (lan_comm_handle_t)calloc(1, sizeof(struct lan_comm_handle_s));
  if (!h) {
    ESP_LOGE(TAG, "Failed to allocate handle");
    return LAN_COMM_ERR_NOMEM;
  }

  // Copy configuration
  memcpy(&h->config, config, sizeof(lan_comm_config_t));

  // Set defaults (fixed 4KB per Section 15.2)
  if (h->config.rx_buffer_size == 0)
    h->config.rx_buffer_size = LAN_COMM_DEFAULT_RX_BUFFER;
  if (h->config.tx_buffer_size == 0)
    h->config.tx_buffer_size = LAN_COMM_DEFAULT_TX_BUFFER;
  if (h->config.dma_channel == 0)
    h->config.dma_channel = SPI_DMA_CH_AUTO;

  // Auto-align buffer sizes
  h->rx_buffer_size_aligned = DMA_ALIGN_SIZE(h->config.rx_buffer_size);
  h->tx_buffer_size_aligned = DMA_ALIGN_SIZE(h->config.tx_buffer_size);

  // Validate DMA descriptor count
  size_t rx_desc_count = calculate_dma_descriptors(h->rx_buffer_size_aligned);
  size_t tx_desc_count = calculate_dma_descriptors(h->tx_buffer_size_aligned);
  if (rx_desc_count > LAN_COMM_MAX_DMA_DESCRIPTORS ||
      tx_desc_count > LAN_COMM_MAX_DMA_DESCRIPTORS) {
    ESP_LOGE(TAG, "Buffer requires too many DMA descriptors");
    ESP_LOGE(TAG, "  RX: %zu descriptors (max %d)", rx_desc_count,
             LAN_COMM_MAX_DMA_DESCRIPTORS);
    ESP_LOGE(TAG, "  TX: %zu descriptors (max %d)", tx_desc_count,
             LAN_COMM_MAX_DMA_DESCRIPTORS);
    free(h);
    return LAN_COMM_ERR_NOMEM;
  }

  ESP_LOGI(TAG, "Buffer Configuration (Section 15.2):");
  ESP_LOGI(TAG, "  RX: %zu -> %zu bytes aligned, %zu DMA descriptors",
           h->config.rx_buffer_size, h->rx_buffer_size_aligned, rx_desc_count);
  ESP_LOGI(TAG, "  TX: %zu -> %zu bytes aligned, %zu DMA descriptors",
           h->config.tx_buffer_size, h->tx_buffer_size_aligned, tx_desc_count);

  // Allocate DMA-aligned buffers
  h->rx_buffer = (uint8_t *)heap_caps_aligned_alloc(
      DMA_ALIGNMENT, h->rx_buffer_size_aligned, MALLOC_CAP_DMA);
  h->tx_buffer = (uint8_t *)heap_caps_aligned_alloc(
      DMA_ALIGNMENT, h->tx_buffer_size_aligned, MALLOC_CAP_DMA);
  if (!h->rx_buffer || !h->tx_buffer) {
    ESP_LOGE(TAG, "Failed to allocate DMA buffers");
    CLEANUP_INIT(h);
    return LAN_COMM_ERR_NOMEM;
  }

  // Verify DMA alignment (critical for QSPI DMA)
  if (!is_dma_aligned(h->rx_buffer, h->rx_buffer_size_aligned) ||
      !is_dma_aligned(h->tx_buffer, h->tx_buffer_size_aligned)) {
    ESP_LOGE(TAG, "DMA buffer alignment verification FAILED");
    ESP_LOGE(TAG, "  RX buffer: %p, size: %zu", h->rx_buffer,
             h->rx_buffer_size_aligned);
    ESP_LOGE(TAG, "  TX buffer: %p, size: %zu", h->tx_buffer,
             h->tx_buffer_size_aligned);
    CLEANUP_INIT(h);
    return LAN_COMM_ERR_DMA_ALIGN;
  }

  ESP_LOGI(TAG, "DMA Buffers Allocated:");
  ESP_LOGI(TAG, "  RX: %p (4-byte aligned)", h->rx_buffer);
  ESP_LOGI(TAG, "  TX: %p (4-byte aligned)", h->tx_buffer);

  // Clear buffers
  memset(h->rx_buffer, 0, h->rx_buffer_size_aligned);
  memset(h->tx_buffer, 0, h->tx_buffer_size_aligned);

  // Create mutexes
  h->rx_mutex = xSemaphoreCreateMutex();
  h->tx_mutex = xSemaphoreCreateMutex();
  if (!h->rx_mutex || !h->tx_mutex) {
    ESP_LOGE(TAG, "Failed to create mutexes");
    CLEANUP_INIT(h);
    return LAN_COMM_ERR_NOMEM;
  }

  // Configure SPI bus
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = config->gpio_io0,
      .miso_io_num = config->gpio_io1,
      .sclk_io_num = config->gpio_sck,
      .quadwp_io_num = config->enable_quad_mode ? config->gpio_io2 : -1,
      .quadhd_io_num = config->enable_quad_mode ? config->gpio_io3 : -1,
      .max_transfer_sz = (h->rx_buffer_size_aligned > h->tx_buffer_size_aligned)
                             ? h->rx_buffer_size_aligned
                             : h->tx_buffer_size_aligned,
      .flags = SPICOMMON_BUSFLAG_SLAVE | SPICOMMON_BUSFLAG_GPIO_PINS};

  // Configure slave HD slot
  spi_slave_hd_slot_config_t slot_cfg = {
      .spics_io_num = config->gpio_cs,
      .flags = SPI_SLAVE_HD_APPEND_MODE, // Enable segmented transactions
      .mode = config->mode,
      .command_bits = 0,
      .address_bits = 0,
      .dummy_bits = 0,
      .queue_size = LAN_COMM_TRANS_QUEUE_SIZE,
      .dma_chan = config->dma_channel,
      .cb_config = {.cb_buffer_tx = NULL,
                    .cb_buffer_rx = NULL,
                    .cb_sent = NULL,
                    .cb_recv = NULL}};

  ESP_LOGI(TAG, "SPI Slave HD Configuration:");
  ESP_LOGI(TAG, "  Host: SPI%d, Mode: %d, Queue Size: %d", config->host_id + 1,
           config->mode, LAN_COMM_TRANS_QUEUE_SIZE);
  ESP_LOGI(TAG, "  GPIO: CLK=%d, CS=%d, IO0=%d, IO1=%d, IO2=%d, IO3=%d",
           config->gpio_sck, config->gpio_cs, config->gpio_io0,
           config->gpio_io1, config->gpio_io2, config->gpio_io3);
  ESP_LOGI(TAG, "  QSPI Mode: %s (4-bit parallel)",
           config->enable_quad_mode ? "Enabled" : "Disabled");

  // Initialize SPI slave HD
  esp_err_t ret = spi_slave_hd_init(config->host_id, &bus_cfg, &slot_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI slave HD: %s",
             esp_err_to_name(ret));
    CLEANUP_INIT(h);
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Configure GPIO CS pull-up
  gpio_set_pull_mode(config->gpio_cs, GPIO_PULLUP_ONLY);

  // Setup data-ready GPIO if specified
  h->gpio_configured = false;
  if (config->gpio_data_ready >= 0) {
    if (setup_data_ready_gpio(config->gpio_data_ready) == ESP_OK) {
      h->gpio_configured = true;
      ESP_LOGI(TAG, "Data-Ready GPIO:");
      ESP_LOGI(TAG, "  Pin: GPIO%d, Pulse Width: %u us, Auto-signal: %s",
               config->gpio_data_ready, LAN_COMM_GPIO_PULSE_US,
               config->auto_signal_data_ready ? "Yes" : "No");
    } else {
      ESP_LOGW(TAG, "Failed to setup GPIO%d, data-ready signaling disabled",
               config->gpio_data_ready);
    }
  } else {
    ESP_LOGI(TAG, "Data-Ready GPIO: Disabled (gpio_data_ready = -1)");
  }

  // Initialize state
  h->is_initialized = true;
  h->rx_queued = false;
  h->tx_loaded = false;
  h->last_error = LAN_COMM_OK;
  h->packets_received = 0;
  h->packets_sent = 0;
  h->frames_parsed = 0;
  h->padding_bytes_skipped = 0;
  h->error_count = 0;

  *handle = h;

  ESP_LOGI(TAG, "============================================");
  ESP_LOGI(TAG, "QSPI Slave Ready - Waiting for 40 MHz Master");
  ESP_LOGI(TAG, "============================================");
  ESP_LOGI(TAG, "Expected Performance: 8-12 MB/s @ 40 MHz QSPI");
  ESP_LOGI(TAG, "Timing: ACK timeout=%dms, DQ retry=%dms×%d",
           LAN_COMM_ACK_TIMEOUT_MS, LAN_COMM_DQ_RETRY_MS,
           LAN_COMM_DQ_RETRY_COUNT);

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Deinitializing QSPI slave");

  // Reset GPIO if configured
  if (handle->gpio_configured && handle->config.gpio_data_ready >= 0) {
    gpio_reset_pin(handle->config.gpio_data_ready);
  }

  // Deinitialize SPI slave HD
  spi_slave_hd_deinit(handle->config.host_id);

  // Free resources
  if (handle->rx_buffer)
    heap_caps_free(handle->rx_buffer);
  if (handle->tx_buffer)
    heap_caps_free(handle->tx_buffer);
  if (handle->rx_mutex)
    vSemaphoreDelete(handle->rx_mutex);
  if (handle->tx_mutex)
    vSemaphoreDelete(handle->tx_mutex);

  // Print final statistics
  ESP_LOGI(TAG, "Final Statistics:");
  ESP_LOGI(TAG, "  Packets: RX=%lu, TX=%lu, Errors=%lu",
           handle->packets_received, handle->packets_sent, handle->error_count);
  ESP_LOGI(TAG, "  Frames parsed: %lu, Padding skipped: %lu bytes",
           handle->frames_parsed, handle->padding_bytes_skipped);

  handle->is_initialized = false;
  free(handle);

  ESP_LOGI(TAG, "QSPI slave deinitialized");
  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  // Lock RX buffer
  if (xSemaphoreTake(handle->rx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "queue_receive mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  // Clear RX buffer
  memset(handle->rx_buffer, 0, handle->rx_buffer_size_aligned);

  // Setup RX transaction
  handle->rx_trans.data = handle->rx_buffer;
  handle->rx_trans.len = handle->rx_buffer_size_aligned;

  // Queue RX transaction (non-blocking)
  esp_err_t ret = spi_slave_hd_queue_trans(
      handle->config.host_id, SPI_SLAVE_CHAN_RX, &handle->rx_trans, 0);
  if (ret == ESP_OK) {
    handle->rx_queued = true;
    ESP_LOGV(TAG, "RX transaction queued: %zu bytes",
             handle->rx_buffer_size_aligned);
  } else if (ret == ESP_ERR_TIMEOUT) {
    ESP_LOGD(TAG, "RX queue full, will retry next cycle");
  } else {
    ESP_LOGE(TAG, "Failed to queue RX transaction: %s", esp_err_to_name(ret));
    xSemaphoreGive(handle->rx_mutex);
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "queue_receive failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  xSemaphoreGive(handle->rx_mutex);
  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_get_received_packet(lan_comm_handle_t handle,
                                               lan_comm_packet_t *packet,
                                               uint32_t timeout_ms) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (!packet) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (!handle->rx_queued) {
    ESP_LOGD(TAG,
             "No RX transaction queued, call lan_comm_queue_receive() first");
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Wait for transaction completion
  spi_slave_hd_data_t *result_trans;
  TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  esp_err_t ret = spi_slave_hd_get_trans_res(
      handle->config.host_id, SPI_SLAVE_CHAN_RX, &result_trans, ticks);

  if (ret == ESP_ERR_TIMEOUT) {
    return LAN_COMM_ERR_TIMEOUT;
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get RX transaction result: %s",
             esp_err_to_name(ret));
    handle->rx_queued = false;
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "get_received_packet failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  handle->rx_queued = false;

  // Check received length
  size_t received_bytes = result_trans->trans_len;
  if (received_bytes == 0) {
    ESP_LOGV(TAG, "Empty QSPI transaction");
    return LAN_COMM_ERR_NO_DATA;
  }

  ESP_LOGD(TAG, "QSPI RX complete: %zu bytes", received_bytes);

  // Parse DMA buffer with 0x00 padding removal (Section 15.2)
  size_t frame_size = 0;
  lan_comm_status_t status = lan_comm_parse_frame(
      handle->rx_buffer, received_bytes, packet, &frame_size);

  if (status == LAN_COMM_OK) {
    handle->packets_received++;
    handle->frames_parsed++;
    ESP_LOGD(
        TAG, "Frame parsed: Header=0x%04X, Payload=%u bytes (total RX=%lu)",
        packet->header_type, packet->payload_length, handle->packets_received);
    return LAN_COMM_OK;
  }

  // No valid frame found (could be all 0x00 padding or invalid data)
  if (status != LAN_COMM_ERR_NO_DATA) {
    lan_comm_report_error(handle, status, "frame parse error");
  }

  return status;
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
    // Skip 0x00 padding (Section 15.2)
    size_t padding_start = offset;
    while (offset < length && buffer[offset] == 0x00) {
      offset++;
    }

    size_t padding_skipped = offset - padding_start;
    if (padding_skipped > 0) {
      handle->padding_bytes_skipped += padding_skipped;
      ESP_LOGV(TAG, "Skipped %zu bytes of 0x00 padding at offset %zu",
               padding_skipped, padding_start);
    }

    if (offset >= length) {
      break; // End of buffer
    }

    // Parse frame
    lan_comm_packet_t packet;
    size_t frame_size = 0;
    lan_comm_status_t status = lan_comm_parse_frame(
        &buffer[offset], length - offset, &packet, &frame_size);

    if (status == LAN_COMM_OK && frame_size > 0) {
      // Valid frame found - call callback
      callback(&packet, user_arg);
      frame_count++;
      handle->frames_parsed++;
      offset += frame_size;
      ESP_LOGV(TAG, "Frame #%lu parsed: offset=%zu, size=%zu, header=0x%04X",
               frame_count, offset - frame_size, frame_size,
               packet.header_type);
    } else {
      // Invalid byte - skip and continue
      offset++;
    }
  }

  ESP_LOGD(TAG, "Parsed %lu frames from %zu bytes buffer", frame_count, length);
  return frame_count;
}

lan_comm_status_t lan_comm_load_tx_data(lan_comm_handle_t handle,
                                        const uint8_t *data_to_send,
                                        uint16_t length) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (!data_to_send || length == 0) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (length > handle->tx_buffer_size_aligned) {
    ESP_LOGE(TAG, "TX data %u bytes exceeds buffer %zu bytes", length,
             handle->tx_buffer_size_aligned);
    return LAN_COMM_ERR_INVALID_ARG;
  }

  // Lock TX buffer
  if (xSemaphoreTake(handle->tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "load_tx_data mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  // Copy data to TX buffer
  memset(handle->tx_buffer, 0, handle->tx_buffer_size_aligned);
  memcpy(handle->tx_buffer, data_to_send, length);

  // Setup TX transaction
  handle->tx_trans.data = handle->tx_buffer;
  handle->tx_trans.len = length;

  // Queue TX transaction (non-blocking)
  esp_err_t ret = spi_slave_hd_queue_trans(
      handle->config.host_id, SPI_SLAVE_CHAN_TX, &handle->tx_trans, 0);

  if (ret == ESP_OK) {
    handle->tx_loaded = true;
    handle->packets_sent++;
    ESP_LOGD(TAG, "QSPI TX queued: %u bytes (total=%lu)", length,
             handle->packets_sent);
  } else {
    ESP_LOGE(TAG, "Failed to queue TX transaction: %s", esp_err_to_name(ret));
    xSemaphoreGive(handle->tx_mutex);
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "load_tx_data failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  xSemaphoreGive(handle->tx_mutex);

  // Auto-signal data-ready if enabled
  if (handle->gpio_configured && handle->config.auto_signal_data_ready) {
    lan_comm_signal_data_ready(handle);
  }

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_signal_data_ready(lan_comm_handle_t handle) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (!handle->gpio_configured || handle->config.gpio_data_ready < 0) {
    ESP_LOGW(TAG, "Data-ready GPIO not configured");
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Send pulse: LOW → delay → HIGH
  // Master detects rising edge with ISR (<5ms response)
  gpio_set_level(handle->config.gpio_data_ready, 0);
  ets_delay_us(LAN_COMM_GPIO_PULSE_US);
  gpio_set_level(handle->config.gpio_data_ready, 1);

  ESP_LOGV(TAG, "Data-ready pulse sent: GPIO%d, %u us",
           handle->config.gpio_data_ready, LAN_COMM_GPIO_PULSE_US);

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle) {
  if (!handle) {
    return LAN_COMM_ERR_INVALID_ARG;
  }
  return handle->last_error;
}

lan_comm_status_t lan_comm_get_statistics(lan_comm_handle_t handle,
                                          uint32_t *packets_received,
                                          uint32_t *errors) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (packets_received)
    *packets_received = handle->packets_received;
  if (errors)
    *errors = handle->error_count;

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_clear_statistics(lan_comm_handle_t handle) {
  if (!handle || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  handle->packets_received = 0;
  handle->packets_sent = 0;
  handle->frames_parsed = 0;
  handle->padding_bytes_skipped = 0;
  handle->error_count = 0;

  ESP_LOGI(TAG, "Statistics cleared");
  return LAN_COMM_OK;
}

/**
 * @brief Parse single frame from DMA buffer (Section 15.2)
 *
 * Skips leading 0x00 padding, extracts first valid frame
 *
 * @param buffer Buffer to parse
 * @param length Buffer length
 * @param[out] packet Parsed packet structure
 * @param[out] frame_size Total frame size (header + payload)
 * @return LAN_COMM_OK if valid frame found
 */
static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size) {
  if (!buffer || length < LAN_COMM_HEADER_SIZE || !packet || !frame_size) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  size_t offset = 0;

  // Skip leading 0x00 padding (Section 15.2)
  while (offset < length && buffer[offset] == 0x00) {
    offset++;
  }

  if (offset >= length) {
    // All padding, no valid frame
    return LAN_COMM_ERR_NO_DATA;
  }

  if ((length - offset) < LAN_COMM_HEADER_SIZE) {
    // Not enough bytes for header
    return LAN_COMM_ERR_NO_DATA;
  }

  // Extract header (big-endian)
  uint16_t header = (buffer[offset] << 8) | buffer[offset + 1];

  // Check for empty transaction (0x0000)
  if (header == 0x0000) {
    return LAN_COMM_ERR_NO_DATA;
  }

  // Check for master polling: CF + zeros
  if (header == LAN_COMM_HEADER_CF) {
    bool is_polling = true;
    for (size_t i = offset + LAN_COMM_HEADER_SIZE;
         i < offset + LAN_COMM_HEADER_SIZE + 10 && i < length; i++) {
      if (buffer[i] != 0x00) {
        is_polling = false;
        break;
      }
    }
    if (is_polling) {
      ESP_LOGV(TAG, "Master polling packet ignored");
      return LAN_COMM_ERR_NO_DATA;
    }
  }

  // Validate header
  if (header != LAN_COMM_HEADER_CF && header != LAN_COMM_HEADER_DT &&
      header != LAN_COMM_HEADER_DQ && header != LAN_COMM_HEADER_CQ) {
    ESP_LOGD(TAG, "Invalid header: 0x%04X at offset %zu", header, offset);
    return LAN_COMM_ERR_INVALID_HEADER;
  }

  // Extract payload
  packet->header_type = header;
  packet->payload = (uint8_t *)&buffer[offset + LAN_COMM_HEADER_SIZE];
  packet->payload_length = (length - offset) - LAN_COMM_HEADER_SIZE;
  *frame_size = length - offset; // Entire remaining buffer treated as one frame

  return LAN_COMM_OK;
}

static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error,
                                  const char *context) {
  if (!handle)
    return;
  handle->last_error = error;
  handle->error_count++;
  ESP_LOGE(TAG, "Error #%lu (code=%d): %s", handle->error_count, error,
           context);
}

static bool is_dma_aligned(const void *ptr, size_t size) {
  uintptr_t addr = (uintptr_t)ptr;
  return (addr % DMA_ALIGNMENT == 0) && (size % DMA_ALIGNMENT == 0);
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

  // Initialize to LOW (idle state)
  gpio_set_level(gpio_pin, 0);
  return ESP_OK;
}

static size_t calculate_dma_descriptors(size_t buffer_size) {
  return (buffer_size + LAN_COMM_DMA_DESCRIPTOR_SIZE - 1) /
         LAN_COMM_DMA_DESCRIPTOR_SIZE;
}
