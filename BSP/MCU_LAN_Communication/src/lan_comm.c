/**
 * @file lan_comm.c
 * @brief LAN MCU Communication Library Implementation
 */

#include "lan_comm.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/task.h"

static const char* TAG = "LAN_COMM";

/**
 * @brief Internal handle structure
 */
struct lan_comm_handle_s {
    // Configuration
    lan_comm_config_t config;
    
    // SPI handle
    spi_device_handle_t spi_device;
    
    // Buffers
    uint8_t* tx_buffer;
    uint8_t* rx_buffer;
    size_t buffer_size;
    
    // Synchronization
    SemaphoreHandle_t transfer_mutex;
    SemaphoreHandle_t transfer_complete_sem;
    
    // State
    bool is_initialized;
    bool is_blocking_mode;
    lan_comm_status_t last_error;
    
    // Error tracking
    uint32_t error_count;
};

// Forward declarations
static void lan_comm_transfer_complete_isr(spi_transaction_t* trans);
static lan_comm_status_t lan_comm_validate_transaction(lan_comm_handle_t handle, uint16_t length);
static void lan_comm_report_error(lan_comm_handle_t handle, lan_comm_status_t error, const char* context);

/**
 * @brief Initialize LAN communication library
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t* config, lan_comm_handle_t* handle) {
    if (config == NULL || handle == NULL) {
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing LAN communication library");
    
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
    
    // Allocate buffers (DMA-capable memory)
    h->buffer_size = LAN_COMM_MAX_TRANSFER_SIZE + LAN_COMM_HEADER_SIZE;
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
    h->transfer_complete_sem = xSemaphoreCreateBinary();
    
    if (h->transfer_mutex == NULL || h->transfer_complete_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives");
        free(h->tx_buffer);
        free(h->rx_buffer);
        if (h->transfer_mutex) vSemaphoreDelete(h->transfer_mutex);
        if (h->transfer_complete_sem) vSemaphoreDelete(h->transfer_complete_sem);
        free(h);
        return LAN_COMM_ERR_NO_MEM;
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
        free(h->tx_buffer);
        free(h->rx_buffer);
        vSemaphoreDelete(h->transfer_mutex);
        vSemaphoreDelete(h->transfer_complete_sem);
        free(h);
        return LAN_COMM_ERR_INVALID_STATE;
    }
    
    // Configure SPI device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = config->clock_speed_hz,
        .mode = config->mode,
        .spics_io_num = config->gpio_cs,
        .queue_size = config->queue_size,
        .flags = config->enable_quad_mode ? SPI_DEVICE_HALFDUPLEX : 0,
        .pre_cb = NULL,
        .post_cb = lan_comm_transfer_complete_isr
    };
    
    ret = spi_bus_add_device(config->host_id, &dev_cfg, &h->spi_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(config->host_id);
        free(h->tx_buffer);
        free(h->rx_buffer);
        vSemaphoreDelete(h->transfer_mutex);
        vSemaphoreDelete(h->transfer_complete_sem);
        free(h);
        return LAN_COMM_ERR_INVALID_STATE;
    }
    
    // Initialize state
    h->is_initialized = true;
    h->is_blocking_mode = true;  // Default to blocking
    h->last_error = LAN_COMM_OK;
    h->error_count = 0;
    
    *handle = h;
    
    ESP_LOGI(TAG, "LAN communication initialized successfully");
    ESP_LOGI(TAG, "Clock: %lu Hz, Mode: %d, Quad: %s", 
             config->clock_speed_hz, config->mode, 
             config->enable_quad_mode ? "Yes" : "No");
    
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
    
    // Remove device
    spi_bus_remove_device(handle->spi_device);
    
    // Free bus
    spi_bus_free(handle->config.host_id);
    
    // Free resources
    free(handle->tx_buffer);
    free(handle->rx_buffer);
    vSemaphoreDelete(handle->transfer_mutex);
    vSemaphoreDelete(handle->transfer_complete_sem);
    
    handle->is_initialized = false;
    free(handle);
    
    ESP_LOGI(TAG, "LAN communication deinitialized");
    
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
    
    lan_comm_status_t status = lan_comm_validate_transaction(handle, length);
    if (status != LAN_COMM_OK) {
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_command mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Build packet: [CF header][payload]
    handle->tx_buffer[0] = (LAN_COMM_HEADER_CF >> 8) & 0xFF;
    handle->tx_buffer[1] = LAN_COMM_HEADER_CF & 0xFF;
    memcpy(&handle->tx_buffer[LAN_COMM_HEADER_SIZE], command_payload, length);
    
    // Prepare transaction
    spi_transaction_t trans = {
        .length = (length + LAN_COMM_HEADER_SIZE) * 8,  // in bits
        .tx_buffer = handle->tx_buffer,
        .rx_buffer = NULL,
        .user = handle
    };
    
    // Transmit
    esp_err_t ret;
    if (handle->is_blocking_mode) {
        ret = spi_device_transmit(handle->spi_device, &trans);
    } else {
        ret = spi_device_queue_trans(handle->spi_device, &trans, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS));
        if (ret == ESP_OK) {
            // Wait for completion in non-blocking mode
            if (xSemaphoreTake(handle->transfer_complete_sem, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
                xSemaphoreGive(handle->transfer_mutex);
                lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_command transfer timeout");
                return LAN_COMM_ERR_TIMEOUT;
            }
        }
    }
    
    xSemaphoreGive(handle->transfer_mutex);
    
    if (ret != ESP_OK) {
        lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "send_command SPI error");
        return LAN_COMM_ERR_BUS_BUSY;
    }
    
    ESP_LOGD(TAG, "Command sent: %d bytes", length);
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
    
    lan_comm_status_t status = lan_comm_validate_transaction(handle, length);
    if (status != LAN_COMM_OK) {
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_data mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Build packet: [DT header][payload]
    handle->tx_buffer[0] = (LAN_COMM_HEADER_DT >> 8) & 0xFF;
    handle->tx_buffer[1] = LAN_COMM_HEADER_DT & 0xFF;
    memcpy(&handle->tx_buffer[LAN_COMM_HEADER_SIZE], data_payload, length);
    
    // Prepare transaction
    spi_transaction_t trans = {
        .length = (length + LAN_COMM_HEADER_SIZE) * 8,  // in bits
        .tx_buffer = handle->tx_buffer,
        .rx_buffer = NULL,
        .user = handle
    };
    
    // Transmit
    esp_err_t ret;
    if (handle->is_blocking_mode) {
        ret = spi_device_transmit(handle->spi_device, &trans);
    } else {
        ret = spi_device_queue_trans(handle->spi_device, &trans, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS));
        if (ret == ESP_OK) {
            // Wait for completion in non-blocking mode
            if (xSemaphoreTake(handle->transfer_complete_sem, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
                xSemaphoreGive(handle->transfer_mutex);
                lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "send_data transfer timeout");
                return LAN_COMM_ERR_TIMEOUT;
            }
        }
    }
    
    xSemaphoreGive(handle->transfer_mutex);
    
    if (ret != ESP_OK) {
        lan_comm_report_error(handle, LAN_COMM_ERR_BUS_BUSY, "send_data SPI error");
        return LAN_COMM_ERR_BUS_BUSY;
    }
    
    ESP_LOGD(TAG, "Data sent: %d bytes", length);
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
    
    lan_comm_status_t status = lan_comm_validate_transaction(handle, length_to_read);
    if (status != LAN_COMM_OK) {
        return status;
    }
    
    // Take mutex
    if (xSemaphoreTake(handle->transfer_mutex, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
        lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "request_data mutex timeout");
        return LAN_COMM_ERR_TIMEOUT;
    }
    
    // Prepare transaction (receive only)
    spi_transaction_t trans = {
        .length = length_to_read * 8,  // in bits
        .rxlength = length_to_read * 8,
        .tx_buffer = NULL,
        .rx_buffer = handle->rx_buffer,
        .user = handle
    };
    
    // Transmit
    esp_err_t ret;
    if (handle->is_blocking_mode) {
        ret = spi_device_transmit(handle->spi_device, &trans);
    } else {
        ret = spi_device_queue_trans(handle->spi_device, &trans, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS));
        if (ret == ESP_OK) {
            // Wait for completion in non-blocking mode
            if (xSemaphoreTake(handle->transfer_complete_sem, pdMS_TO_TICKS(LAN_COMM_TIMEOUT_MS)) != pdTRUE) {
                xSemaphoreGive(handle->transfer_mutex);
                lan_comm_report_error(handle, LAN_COMM_ERR_TIMEOUT, "request_data transfer timeout");
                return LAN_COMM_ERR_TIMEOUT;
            }
        }
    }
    
    if (ret == ESP_OK) {
        // Copy to user buffer
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
 * @brief Set blocking/non-blocking mode
 */
lan_comm_status_t lan_comm_set_blocking_mode(lan_comm_handle_t handle, bool blocking) {
    if (handle == NULL || !handle->is_initialized) {
        return LAN_COMM_ERR_NOT_INITIALIZED;
    }
    
    handle->is_blocking_mode = blocking;
    ESP_LOGI(TAG, "Transfer mode set to: %s", blocking ? "BLOCKING" : "NON-BLOCKING");
    
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

// ===== Internal Functions =====

/**
 * @brief Transfer complete ISR callback
 */
static void lan_comm_transfer_complete_isr(spi_transaction_t* trans) {
    if (trans->user != NULL) {
        lan_comm_handle_t handle = (lan_comm_handle_t)trans->user;
        
        // Signal completion for non-blocking mode
        if (!handle->is_blocking_mode) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            xSemaphoreGiveFromISR(handle->transfer_complete_sem, &higher_priority_task_woken);
            
            // Call user callback if registered
            if (handle->config.transfer_callback != NULL) {
                handle->config.transfer_callback(LAN_COMM_OK, handle->config.user_data);
            }
            
            if (higher_priority_task_woken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

/**
 * @brief Validate transaction parameters
 */
static lan_comm_status_t lan_comm_validate_transaction(lan_comm_handle_t handle, uint16_t length) {
    if (length > LAN_COMM_MAX_TRANSFER_SIZE) {
        ESP_LOGE(TAG, "Transfer size %d exceeds maximum %d", length, LAN_COMM_MAX_TRANSFER_SIZE);
        return LAN_COMM_ERR_INVALID_ARG;
    }
    
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
    
    ESP_LOGE(TAG, "Error: %d, Context: %s", error, context);
    
    // Call user error callback if registered
    if (handle->config.error_callback != NULL) {
        handle->config.error_callback(error, context, handle->config.user_data);
    }
}
