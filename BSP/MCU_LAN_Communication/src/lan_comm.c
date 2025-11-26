/**
 * @file lan_comm.c
 * @brief LAN Communication Library Implementation (Slave - API only)
 */

#include "lan_comm.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "LAN_COMM_SLAVE";

/**
 * @brief Internal handle structure
 */
struct lan_comm_handle_s {
  // Configuration
  lan_comm_config_t config;

  // SPI slave transaction
  spi_slave_transaction_t spi_trans;

  // Buffers
  uint8_t *rx_buffer;
  uint8_t *tx_buffer;
  size_t tx_buffer_len;
  SemaphoreHandle_t buffer_mutex;

  // State
  bool is_initialized;
  bool transaction_queued;
  lan_comm_status_t last_error;

  // Statistics
  uint32_t packets_received;
  uint32_t error_count;
};

// Forward declarations
static lan_comm_status_t lan_comm_parse_packet(uint8_t *buffer, size_t length,
                                               lan_comm_packet_t *packet);
static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error, const char *context);

/**
 * @brief Initialize LAN communication library
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t *config,
                                lan_comm_handle_t *handle) {
  if (config == NULL || handle == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG,
           "Initializing LAN communication library (Slave mode for WAN MCU)");

  // Allocate handle
  lan_comm_handle_t h =
      (lan_comm_handle_t)calloc(1, sizeof(struct lan_comm_handle_s));
  if (h == NULL) {
    ESP_LOGE(TAG, "Failed to allocate handle");
    return LAN_COMM_ERR_NO_MEM;
  }

  // Copy configuration
  memcpy(&h->config, config, sizeof(lan_comm_config_t));

  // Set defaults
  if (h->config.rx_buffer_size == 0) {
    h->config.rx_buffer_size = LAN_COMM_DEFAULT_RX_BUFFER_SIZE;
  }
  if (h->config.tx_buffer_size == 0) {
    h->config.tx_buffer_size = LAN_COMM_DEFAULT_TX_BUFFER_SIZE;
  }
  if (h->config.dma_channel == 0) {
    h->config.dma_channel = SPI_DMA_CH_AUTO;
  }

  // Allocate buffers (DMA-capable memory)
  h->rx_buffer =
      (uint8_t *)heap_caps_malloc(h->config.rx_buffer_size, MALLOC_CAP_DMA);
  h->tx_buffer =
      (uint8_t *)heap_caps_malloc(h->config.tx_buffer_size, MALLOC_CAP_DMA);
  h->buffer_mutex = xSemaphoreCreateMutex();
  h->tx_buffer_len = 0;

  if (h->rx_buffer == NULL || h->tx_buffer == NULL || h->buffer_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to allocate buffers or mutex");
    free(h->rx_buffer);
    free(h->tx_buffer);
    if (h->buffer_mutex)
      vSemaphoreDelete(h->buffer_mutex);
    free(h);
    return LAN_COMM_ERR_NO_MEM;
  }

  // Clear buffers
  memset(h->rx_buffer, 0, h->config.rx_buffer_size);
  memset(h->tx_buffer, 0, h->config.tx_buffer_size);

  // Configure SPI bus
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = config->gpio_io0,
      .miso_io_num = config->gpio_io1,
      .sclk_io_num = config->gpio_sck,
      .quadwp_io_num = config->enable_quad_mode ? config->gpio_io2 : -1,
      .quadhd_io_num = config->enable_quad_mode ? config->gpio_io3 : -1,
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
                                       config->dma_channel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI slave: %s", esp_err_to_name(ret));
    free(h->rx_buffer);
    free(h->tx_buffer);
    vSemaphoreDelete(h->buffer_mutex);
    free(h);
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Configure GPIO for CS
  gpio_set_pull_mode(config->gpio_cs, GPIO_PULLUP_ONLY);

  // Initialize state
  h->is_initialized = true;
  h->transaction_queued = false;
  h->last_error = LAN_COMM_OK;
  h->packets_received = 0;
  h->error_count = 0;

  *handle = h;
  ESP_LOGI(TAG, "LAN communication initialized successfully (WAN MCU Slave)");
  ESP_LOGI(TAG, "Mode: %d, RX Buffer: %d bytes, TX Buffer: %d bytes",
           config->mode, h->config.rx_buffer_size, h->config.tx_buffer_size);

  return LAN_COMM_OK;
}

/**
 * @brief Deinitialize LAN communication library
 */
lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Deinitializing LAN communication library");

  // Free SPI slave
  spi_slave_free(handle->config.host_id);

  // Free resources
  free(handle->rx_buffer);
  free(handle->tx_buffer);
  vSemaphoreDelete(handle->buffer_mutex);

  handle->is_initialized = false;
  free(handle);

  ESP_LOGI(TAG, "LAN communication deinitialized");
  return LAN_COMM_OK;
}

/**
 * @brief Queue a receive transaction
 */
lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (handle->transaction_queued) {
    ESP_LOGW(TAG, "Transaction already queued");
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Lock buffer
  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "queue_receive mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  // Clear RX buffer
  memset(handle->rx_buffer, 0, handle->config.rx_buffer_size);

  // Setup transaction
  memset(&handle->spi_trans, 0, sizeof(spi_slave_transaction_t));
  handle->spi_trans.length = handle->config.rx_buffer_size * 8; // in bits
  handle->spi_trans.rx_buffer = handle->rx_buffer;
  handle->spi_trans.tx_buffer = handle->tx_buffer;

  // Queue transaction
  esp_err_t ret =
      spi_slave_queue_trans(handle->config.host_id, &handle->spi_trans, 0);

  xSemaphoreGive(handle->buffer_mutex);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to queue SPI transaction: %s", esp_err_to_name(ret));
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "queue_receive failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  handle->transaction_queued = true;
  ESP_LOGD(TAG, "Receive transaction queued");

  return LAN_COMM_OK;
}

/**
 * @brief Get received packet (non-blocking check)
 */
lan_comm_status_t lan_comm_get_received_packet(lan_comm_handle_t handle,
                                               lan_comm_packet_t *packet,
                                               uint32_t timeout_ms) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (packet == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (!handle->transaction_queued) {
    ESP_LOGW(TAG, "No transaction queued, call lan_comm_queue_receive() first");
    return LAN_COMM_ERR_INVALID_STATE;
  }

  // Wait for transaction completion
  spi_slave_transaction_t *trans;
  TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);

  esp_err_t ret =
      spi_slave_get_trans_result(handle->config.host_id, &trans, ticks);

  if (ret == ESP_ERR_TIMEOUT) {
    return LAN_COMM_ERR_TIMEOUT;
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get transaction result: %s", esp_err_to_name(ret));
    handle->transaction_queued = false;
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "get_received_packet failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  handle->transaction_queued = false;

  // Check if data was received
  if (trans->trans_len == 0) {
    ESP_LOGD(TAG, "Empty transaction");
    return LAN_COMM_ERR_NO_DATA;
  }

  // Convert trans_len from bits to bytes
  size_t received_bytes = trans->trans_len / 8;
  ESP_LOGI(TAG, "Transaction complete: %d bytes received", received_bytes);

  // Parse packet
  lan_comm_status_t status =
      lan_comm_parse_packet(handle->rx_buffer, received_bytes, packet);

  if (status != LAN_COMM_OK) {
    lan_comm_report_error(handle, status, "packet parse error");
    return status;
  }

  handle->packets_received++;
  ESP_LOGI(TAG, "Packet parsed: Header=0x%04X, Payload=%d bytes",
           packet->header_type, packet->payload_length);

  return LAN_COMM_OK;
}

/**
 * @brief Load TX data
 */
lan_comm_status_t lan_comm_load_tx_data(lan_comm_handle_t handle,
                                        const uint8_t *data_to_send,
                                        uint16_t length) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (data_to_send == NULL || length == 0) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (length > handle->config.tx_buffer_size) {
    ESP_LOGE(TAG, "TX data length %d exceeds buffer size %d", length,
             handle->config.tx_buffer_size);
    return LAN_COMM_ERR_INVALID_ARG;
  }

  // Lock TX buffer
  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "load_tx_data mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  // Copy data to TX buffer
  memcpy(handle->tx_buffer, data_to_send, length);
  handle->tx_buffer_len = length;

  xSemaphoreGive(handle->buffer_mutex);

  ESP_LOGI(TAG, "TX data loaded: %d bytes", length);
  return LAN_COMM_OK;
}

/**
 * @brief Get last error
 */
lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle) {
  if (handle == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }
  return handle->last_error;
}

/**
 * @brief Get statistics
 */
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

/**
 * @brief Clear statistics
 */
lan_comm_status_t lan_comm_clear_statistics(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  handle->packets_received = 0;
  handle->error_count = 0;

  return LAN_COMM_OK;
}

// ===== Internal Functions =====

/**
 * @brief Parse packet
 */
static lan_comm_status_t lan_comm_parse_packet(uint8_t *buffer, size_t length,
                                               lan_comm_packet_t *packet) {
  if (buffer == NULL || length < LAN_COMM_HEADER_SIZE || packet == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  // Extract header (first 2 bytes)
  packet->header_type = (buffer[0] << 8) | buffer[1];

  // Validate header
  if (packet->header_type != LAN_COMM_HEADER_CF &&
      packet->header_type != LAN_COMM_HEADER_DT) {
    ESP_LOGE(TAG, "Invalid header: 0x%04X", packet->header_type);
    return LAN_COMM_ERR_INVALID_HEADER;
  }

  // Extract payload
  packet->payload = &buffer[LAN_COMM_HEADER_SIZE];
  packet->payload_length = length - LAN_COMM_HEADER_SIZE;

  return LAN_COMM_OK;
}

/**
 * @brief Report error
 */
static void lan_comm_report_error(lan_comm_handle_t handle,
                                  lan_comm_status_t error,
                                  const char *context) {
  if (handle == NULL) {
    return;
  }

  handle->last_error = error;
  handle->error_count++;
  ESP_LOGE(TAG, "Error: %d, Context: %s", error, context);
}
