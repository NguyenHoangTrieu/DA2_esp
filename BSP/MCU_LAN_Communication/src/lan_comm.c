/**
 * @file lan_comm.c
 * @brief LAN MCU Communication Library Implementation (SPI Master)
 */

#include "lan_comm.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char* TAG = "LAN_COMM";

/**
 * @brief Internal handle structure
 */
struct lan_comm_handle_s {
    // Configuration
    lan_comm_config_t config;
    
    // SPI handle
    spi_device_handle_t spi_device;
    
    // Buffers (DMA-capable)
    uint8_t* tx_buffer;
    uint8_t* rx_buffer;
    size_t buffer_size;
    
    // Synchronization
    SemaphoreHandle_t transfer_mutex;
    QueueHandle_t rdy_sem;  // Handshake ready semaphore
    
    // State
    bool is_initialized;
    lan_comm_status_t last_error;
    
    // Error tracking
    uint32_t error_count;
    uint32_t transaction_count;
};

// Forward declarations
static void IRAM_ATTR lan_comm_handshake_isr(void* arg);
static lan_comm_status_t lan_comm_wait_for_slave_ready(lan_comm_handle_t handle);
static void lan_comm_report_error(lan_comm_handle_t handle, lan_comm_status_t error, const char* context);

/**
 * @brief GPIO handshake ISR - called when slave is ready with debouncing
 */
static void IRAM_ATTR lan_comm_handshake_isr(void* arg) {
    lan_comm_handle_t handle = (lan_comm_handle_t)arg;
    
    // Debouncing: ignore interrupts < 1ms apart
    static uint32_t last_handshake_time_us = 0;
    uint32_t curr_time_us = esp_timer_get_time();
    uint32_t diff = curr_time_us - last_handshake_time_us;
    
    if (diff < 1000) {  // Ignore everything < 1ms after an earlier IRQ
        return;
    }
    last_handshake_time_us = curr_time_us;
    
    // Give the semaphore to signal slave is ready
    BaseType_t must_yield = pdFALSE;
    xSemaphoreGiveFromISR(handle->rdy_sem, &must_yield);
    
    if (must_yield) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Initialize LAN communication library
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t* config, lan_comm_handle_t* handle) {
    if (config == NULL || handle == NULL) {
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing LAN communication library (Master mode)");
    
    // Allocate handle
    lan_comm_handle_t h = (lan_comm_handle_t)calloc(1, sizeof(struct lan_comm_handle_s));
    if (h == NULL) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        return LAN_COMM_ERR_NO_MEM;
    }
    
    // Copy configuration
    memcpy(&h->config, config, sizeof(lan_comm_config_t));
    
    // Set defaults
    if (h->config.clock_speed_hz == 0) {
        h->config.clock_speed_hz = LAN_COMM_DEFAULT_CLOCK_HZ;
    }
    if (h->config.queue_size == 0) {
        h->config.queue_size = LAN_COMM_DEFAULT_QUEUE_SIZE;
    }
    if (h->config.dma_channel == 0) {
        h->config.dma_channel = SPI_DMA_CH_AUTO;
    }
    
    // Allocate DMA-capable buffers
    h->buffer_size = LAN_COMM_FIXED_TRANSFER_SIZE;
    h->tx_buffer = (uint8_t*)heap_caps_malloc(h->buffer_size, MALLOC_CAP_DMA);
    h->rx_buffer = (uint8_t*)heap_caps_malloc(h->buffer_size, MALLOC_CAP_DMA);
    
    if (h->tx_buffer == NULL || h->rx_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffers");
        free(h->tx_buffer);
        free(h->rx_buffer);
        free(h);
        return LAN_COMM_ERR_NO_MEM;
    }
    
    // Create synchronization primitives
    h->transfer_mutex = xSemaphoreCreateMutex();
    h->rdy_sem = xSemaphoreCreateBinary();
    
    if (h->transfer_mutex == NULL || h->rdy_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives");
        free(h->tx_buffer);
        free(h->rx_buffer);
        if (h->transfer_mutex) vSemaphoreDelete(h->transfer_mutex);
        if (h->rdy_sem) vSemaphoreDelete(h->rdy_sem);
        free(h);
        return LAN_COMM_ERR_NO_MEM;
    }
    
    // Configure handshake GPIO as input with interrupt
    if (config->gpio_handshake >= 0) {
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_POSEDGE,  // Trigger on rising edge
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = (1ULL << config->gpio_handshake),
            .pull_down_en = 0,
            .pull_up_en = 1
        };
        gpio_config(&io_conf);
        
        // Install ISR service and add handler
        gpio_install_isr_service(0);
        gpio_isr_handler_add(config->gpio_handshake, lan_comm_handshake_isr, h);
        
        ESP_LOGI(TAG, "Handshake GPIO %d configured with ISR", config->gpio_handshake);
    }
    
    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = config->gpio_io0,
        .miso_io_num = config->gpio_io1,
        .sclk_io_num = config->gpio_sck,
        .quadwp_io_num = config->enable_quad_mode ? config->gpio_io2 : -1,
        .quadhd_io_num = config->enable_quad_mode ? config->gpio_io3 : -1,
        .max_transfer_sz = h->buffer_size,
        .flags = SPICOMMON_BUSFLAG_MASTER
    };
    
    esp_err_t ret = spi_bus_initialize(config->host_id, &bus_cfg, config->dma_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        if (config->gpio_handshake >= 0) {
            gpio_isr_handler_remove(config->gpio_handshake);
        }
        free(h->tx_buffer);
        free(h->rx_buffer);
        vSemaphoreDelete(h->transfer_mutex);
        vSemaphoreDelete(h->rdy_sem);
        free(h);
        return LAN_COMM_ERR_INVALID_STATE;
    }
    
    // Configure SPI device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = config->clock_speed_hz,
        .mode = config->mode,
        .spics_io_num = config->gpio_cs,
        .queue_size = config->queue_size,
        .flags = 0,
        .cs_ena_posttrans = 3,    // Keep CS low 3 cycles after transaction
        .duty_cycle_pos = 128     // 50% duty cycle
    };
    
    ret = spi_bus_add_device(config->host_id, &dev_cfg, &h->spi_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(config->host_id);
        if (config->gpio_handshake >= 0) {
            gpio_isr_handler_remove(config->gpio_handshake);
        }
        free(h->tx_buffer);
        free(h->rx_buffer);
        vSemaphoreDelete(h->transfer_mutex);
        vSemaphoreDelete(h->rdy_sem);
        free(h);
        return LAN_COMM_ERR_INVALID_STATE;
    }
    
    // Initialize state
    h->is_initialized = true;
    h->last_error = LAN_COMM_OK;
    h->error_count = 0;
    h->transaction_count = 0;
    
    // Assume slave is ready for first transmission
    // If slave started before master, we won't detect the initial positive edge
    xSemaphoreGive(h->rdy_sem);
    
    *handle = h;
    
    ESP_LOGI(TAG, "LAN communication initialized successfully");
    ESP_LOGI(TAG, "Clock: %lu Hz, Mode: %d, Queue: %d", 
             config->clock_speed_hz, config->mode, config->queue_size);
    
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
    
    // Remove ISR handler
    if (handle->config.gpio_handshake >= 0) {
        gpio_isr_handler_remove(handle->config.gpio_handshake);
    }
    
    // Remove SPI device and free bus
    spi_bus_remove_device(handle->spi_device);
    spi_bus_free(handle->config.host_id);
    
    // Free resources
    free(handle->tx_buffer);
    free(handle->rx_buffer);
    vSemaphoreDelete(handle->transfer_mutex);
    vSemaphoreDelete(handle->rdy_sem);
    
    handle->is_initialized = false;
    free(handle);
    
    ESP_LOGI(TAG, "LAN communication deinitialized");
    return LAN_COMM_OK;
}

/**
 * @brief Wait for slave to be ready (blocking)
 */
static lan_comm_status_t lan_comm_wait_for_slave_ready(lan_comm_handle_t handle) {
    // Wait until slave is ready (handshake semaphore given by ISR)
    if (xSemaphoreTake(handle->rdy_sem, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Timeout waiting for slave ready");
        return LAN_COMM_ERR_TIMEOUT;
    }
    return LAN_COMM_OK;
}

/**
 * @brief Send command packet
 */
lan_comm_status_t lan_comm_send_command(lan_comm_handle_t handle,
                                        const uint8_t* command_payload,
                                        uint16_t length) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    if (command_payload == NULL || length == 0) {
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    if (length > (LAN_COMM_FIXED_TRANSFER_SIZE - LAN_COMM_HEADER_SIZE)) {
        ESP_LOGE(TAG, "Payload too large: %d > %d", length, 
                 LAN_COMM_FIXED_TRANSFER_SIZE - LAN_COMM_HEADER_SIZE);
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    // Wait for slave to be ready (using handshake mechanism)
    lan_comm_status_t status = lan_comm_wait_for_slave_ready(handle);
    if (status != LAN_COMM_OK) {
        lan_comm_report_error(handle, status, "slave not ready for command");
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_command mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Build packet with fixed size (zero-padded)
    memset(handle->tx_buffer, 0, LAN_COMM_FIXED_TRANSFER_SIZE);
    handle->tx_buffer[0] = (LAN_COMM_HEADER_CF >> 8) & 0xFF;
    handle->tx_buffer[1] = LAN_COMM_HEADER_CF & 0xFF;
    memcpy(&handle->tx_buffer[LAN_COMM_HEADER_SIZE], command_payload, length);
    
    // Clear RX buffer
    memset(handle->rx_buffer, 0, LAN_COMM_FIXED_TRANSFER_SIZE);
    
    // Prepare SPI transaction
    spi_transaction_t trans = {
        .length = LAN_COMM_FIXED_TRANSFER_SIZE * 8,    // in bits
        .rxlength = LAN_COMM_FIXED_TRANSFER_SIZE * 8,
        .tx_buffer = handle->tx_buffer,
        .rx_buffer = handle->rx_buffer,  // Full-duplex
        .user = handle
    };
    
    // Transmit
    esp_err_t ret = spi_device_transmit(handle->spi_device, &trans);
    
    xSemaphoreGive(handle->transfer_mutex);
    
    if (ret != ESP_OK) {
        lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "send_command SPI error");
        return LAN_COMM_ERR_BUS_BUSY;
    }
    
    handle->transaction_count++;
    ESP_LOGI(TAG, "Command sent #%lu: %d bytes payload (padded to %d)", 
             handle->transaction_count, length, LAN_COMM_FIXED_TRANSFER_SIZE);
    
    return LAN_COMM_OK;
}

/**
 * @brief Send data packet
 */
lan_comm_status_t lan_comm_send_data(lan_comm_handle_t handle,
                                     const uint8_t* data_payload,
                                     uint16_t length) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    if (data_payload == NULL || length == 0) {
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    if (length > (LAN_COMM_FIXED_TRANSFER_SIZE - LAN_COMM_HEADER_SIZE)) {
        ESP_LOGE(TAG, "Payload too large: %d > %d", length,
                 LAN_COMM_FIXED_TRANSFER_SIZE - LAN_COMM_HEADER_SIZE);
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    // Wait for slave to be ready
    lan_comm_status_t status = lan_comm_wait_for_slave_ready(handle);
    if (status != LAN_COMM_OK) {
        lan_comm_report_error(handle, status, "slave not ready for data");
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_data mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Build packet
    memset(handle->tx_buffer, 0, LAN_COMM_FIXED_TRANSFER_SIZE);
    handle->tx_buffer[0] = (LAN_COMM_HEADER_DT >> 8) & 0xFF;
    handle->tx_buffer[1] = LAN_COMM_HEADER_DT & 0xFF;
    memcpy(&handle->tx_buffer[LAN_COMM_HEADER_SIZE], data_payload, length);
    
    // Clear RX buffer
    memset(handle->rx_buffer, 0, LAN_COMM_FIXED_TRANSFER_SIZE);
    
    // Prepare transaction
    spi_transaction_t trans = {
        .length = LAN_COMM_FIXED_TRANSFER_SIZE * 8,
        .rxlength = LAN_COMM_FIXED_TRANSFER_SIZE * 8,
        .tx_buffer = handle->tx_buffer,
        .rx_buffer = handle->rx_buffer,
        .user = handle
    };
    
    // Transmit
    esp_err_t ret = spi_device_transmit(handle->spi_device, &trans);
    
    xSemaphoreGive(handle->transfer_mutex);
    
    if (ret != ESP_OK) {
        lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "send_data SPI error");
        return LAN_COMM_ERR_BUS_BUSY;
    }
    
    handle->transaction_count++;
    ESP_LOGD(TAG, "Data sent #%lu: %d bytes", handle->transaction_count, length);
    
    return LAN_COMM_OK;
}

/**
 * @brief Request data from WAN MCU
 */
lan_comm_status_t lan_comm_request_data(lan_comm_handle_t handle,
                                        uint8_t* rx_buffer,
                                        uint16_t length_to_read) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    if (rx_buffer == NULL || length_to_read == 0) {
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    if (length_to_read > LAN_COMM_FIXED_TRANSFER_SIZE) {
        ESP_LOGE(TAG, "Read length %d exceeds max %d", length_to_read, LAN_COMM_FIXED_TRANSFER_SIZE);
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    // Wait for slave ready
    lan_comm_status_t status = lan_comm_wait_for_slave_ready(handle);
    if (status != LAN_COMM_OK) {
        lan_comm_report_error(handle, status, "slave not ready for read");
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "request_data mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Clear buffers
    memset(handle->tx_buffer, 0, length_to_read);
    memset(handle->rx_buffer, 0, length_to_read);
    
    // Prepare transaction (master clocks, slave sends)
    spi_transaction_t trans = {
        .length = length_to_read * 8,
        .rxlength = length_to_read * 8,
        .tx_buffer = handle->tx_buffer,  // Send dummy data
        .rx_buffer = handle->rx_buffer,
        .user = handle
    };
    
    // Transmit
    esp_err_t ret = spi_device_transmit(handle->spi_device, &trans);
    
    if (ret == ESP_OK) {
        memcpy(rx_buffer, handle->rx_buffer, length_to_read);
    }
    
    xSemaphoreGive(handle->transfer_mutex);
    
    if (ret != ESP_OK) {
        lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "request_data SPI error");
        return LAN_COMM_ERR_BUS_BUSY;
    }
    
    ESP_LOGD(TAG, "Data received: %d bytes", length_to_read);
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
 * @brief Register error callback
 */
lan_comm_status_t lan_comm_register_error_callback(lan_comm_handle_t handle,
                                                   lan_comm_error_cb_t callback) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    handle->config.error_callback = callback;
    return LAN_COMM_OK;
}

/**
 * @brief Get error count
 */
uint32_t lan_comm_get_error_count(lan_comm_handle_t handle) {
    if (handle == NULL) {
        return 0;
    }
    return handle->error_count;
}

/**
 * @brief Clear error count
 */
lan_comm_status_t lan_comm_clear_error_count(lan_comm_handle_t handle) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    handle->error_count = 0;
    return LAN_COMM_OK;
}

/**
 * @brief Report error
 */
static void lan_comm_report_error(lan_comm_handle_t handle,
                                 lan_comm_status_t error,
                                 const char* context) {
    if (handle == NULL) {
        return;
    }
    
    handle->last_error = error;
    handle->error_count++;
    
    ESP_LOGE(TAG, "Error #%lu: %d, Context: %s", handle->error_count, error, context);
    
    // Call user error callback if registered
    if (handle->config.error_callback != NULL) {
        handle->config.error_callback(error, context, handle->config.user_data);
    }
}
