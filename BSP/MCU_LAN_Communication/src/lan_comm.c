#include "lan_comm.h"
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

  spi_slave_transaction_t spi_trans;

  uint8_t *rx_buffer;
  uint8_t *tx_buffer;
  size_t tx_buffer_len;
  SemaphoreHandle_t buffer_mutex;

  bool is_initialized;
  bool transaction_queued;
  bool gpio_configured;
  lan_comm_status_t last_error;

  uint32_t packets_received;
  uint32_t packets_sent;
  uint32_t error_count;
};

static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size);
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

  h->rx_buffer =
      (uint8_t *)heap_caps_malloc(h->config.rx_buffer_size, MALLOC_CAP_DMA);
  h->tx_buffer =
      (uint8_t *)heap_caps_malloc(h->config.tx_buffer_size, MALLOC_CAP_DMA);
  h->buffer_mutex = xSemaphoreCreateMutex();
  h->tx_buffer_len = 0;

  if (h->rx_buffer == NULL || h->tx_buffer == NULL ||
      h->buffer_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to allocate buffers or mutex");
    if (h->rx_buffer)
      heap_caps_free(h->rx_buffer);
    if (h->tx_buffer)
      heap_caps_free(h->tx_buffer);
    if (h->buffer_mutex)
      vSemaphoreDelete(h->buffer_mutex);
    free(h);
    return LAN_COMM_ERR_NOMEM;
  }

  memset(h->rx_buffer, 0, h->config.rx_buffer_size);
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
    heap_caps_free(h->rx_buffer);
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
  h->transaction_queued = false;
  h->last_error = LAN_COMM_OK;
  h->packets_received = 0;
  h->packets_sent = 0;
  h->error_count = 0;

  *handle = h;

  ESP_LOGI(TAG, "SPI slave ready (full-duplex)");
  ESP_LOGI(TAG, "RX buffer: %u bytes, TX buffer: %u bytes",
           (unsigned)h->config.rx_buffer_size,
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

  heap_caps_free(handle->rx_buffer);
  heap_caps_free(handle->tx_buffer);
  vSemaphoreDelete(handle->buffer_mutex);

  handle->is_initialized = false;
  free(handle);

  return LAN_COMM_OK;
}

lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle) {
  if (handle == NULL || !handle->is_initialized) {
    return LAN_COMM_ERR_NOT_INITIALIZED;
  }

  if (handle->transaction_queued) {
    ESP_LOGD(TAG, "RX transaction already queued");
    return LAN_COMM_OK;
  }

  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "queue_receive mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  memset(handle->rx_buffer, 0, handle->config.rx_buffer_size);

  memset(&handle->spi_trans, 0, sizeof(spi_slave_transaction_t));
  handle->spi_trans.length = handle->config.rx_buffer_size * 8;
  handle->spi_trans.rx_buffer = handle->rx_buffer;
  handle->spi_trans.tx_buffer = handle->tx_buffer;

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
  return LAN_COMM_OK;
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

  if (!handle->transaction_queued) {
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
    handle->transaction_queued = false;
    lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY,
                          "get_received_packet failed");
    return LAN_COMM_ERR_BUS_BUSY;
  }

  handle->transaction_queued = false;

  if (trans->trans_len == 0) {
    return LAN_COMM_ERR_NO_DATA;
  }

  size_t received_bytes = trans->trans_len / 8;

  size_t frame_size = 0;
  lan_comm_status_t status =
      lan_comm_parse_frame(handle->rx_buffer, received_bytes, packet,
                           &frame_size);

  if (status != LAN_COMM_OK) {
    if (status != LAN_COMM_ERR_NO_DATA) {
      lan_comm_report_error(handle, status, "packet parse error");
    }
    return status;
  }

  handle->packets_received++;
  return LAN_COMM_OK;
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
                             &frame_size);
    if (status == LAN_COMM_OK && frame_size > 0) {
      callback(&packet, user_arg);
      frame_count++;
      offset += frame_size;
    } else {
      offset++;
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

  if (length > handle->config.tx_buffer_size) {
    ESP_LOGE(TAG, "TX data length %u exceeds buffer size %u", length,
             (unsigned)handle->config.tx_buffer_size);
    return LAN_COMM_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(handle->buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT,
                          "load_tx_data mutex timeout");
    return LAN_COMM_ERR_TIMEOUT;
  }

  spi_slave_disable(handle->config.host_id);

  memset(handle->tx_buffer, 0, handle->config.tx_buffer_size);
  memcpy(handle->tx_buffer, data_to_send, length);
  handle->tx_buffer_len = length;

  spi_slave_enable(handle->config.host_id);

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

static lan_comm_status_t lan_comm_parse_frame(const uint8_t *buffer,
                                              size_t length,
                                              lan_comm_packet_t *packet,
                                              size_t *frame_size) {
  if (buffer == NULL || length < LAN_COMM_HEADER_SIZE || packet == NULL ||
      frame_size == NULL) {
    return LAN_COMM_ERR_INVALID_ARG;
  }

  size_t offset = 0;
  while (offset < length && buffer[offset] == 0x00) {
    offset++;
  }

  if (offset >= length || (length - offset) < LAN_COMM_HEADER_SIZE) {
    return LAN_COMM_ERR_NO_DATA;
  }

  uint16_t header = (buffer[offset] << 8) | buffer[offset + 1];
  if (header == 0x0000) {
    return LAN_COMM_ERR_NO_DATA;
  }

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
      return LAN_COMM_ERR_NO_DATA;
    }
  }

  if (header != LAN_COMM_HEADER_CF && header != LAN_COMM_HEADER_DT &&
      header != LAN_COMM_HEADER_DQ && header != LAN_COMM_HEADER_CQ) {
    return LAN_COMM_ERR_INVALID_HEADER;
  }

  packet->header_type = header;
  // Keep header bytes in payload for current uplink/downlink parsing logic.
  packet->payload = (uint8_t *)&buffer[offset];
  packet->payload_length = length - offset;
  *frame_size = length - offset;

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
