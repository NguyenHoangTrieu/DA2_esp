#ifndef LAN_COMM_H
#define LAN_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_slave.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAN_COMM_DEFAULT_RX_BUFFER    16384     // 16KB – large enough for max config JSON
#define LAN_COMM_DEFAULT_TX_BUFFER    16384     // 16KB
#define LAN_COMM_TRANS_QUEUE_SIZE     7
#define LAN_COMM_ACK_TIMEOUT_MS       500
#define LAN_COMM_DQ_RETRY_MS          50        //
#define LAN_COMM_DQ_RETRY_COUNT       10
#define LAN_COMM_GPIO_PULSE_US        100
#define LAN_COMM_DMA_DESCRIPTOR_SIZE  4092
#define LAN_COMM_MAX_DMA_DESCRIPTORS  8

// Frame headers
#define LAN_COMM_HEADER_CF            0x4346    // Command Frame
#define LAN_COMM_HEADER_DT            0x4454    // Data Transfer
#define LAN_COMM_HEADER_DQ            0x4451    // Data Query
#define LAN_COMM_HEADER_CQ            0x4351    // Config Query
#define LAN_COMM_HEADER_SIZE          2

// DMA alignment
#define DMA_ALIGNMENT                 4
#define DMA_ALIGN_SIZE(x)             (((x) + (DMA_ALIGNMENT - 1)) & ~(DMA_ALIGNMENT - 1))

typedef enum {
    LAN_COMM_OK = 0,
    LAN_COMM_ERR_INVALID_ARG,
    LAN_COMM_ERR_NOT_INITIALIZED,
    LAN_COMM_ERR_NOMEM,
    LAN_COMM_ERR_TIMEOUT,
    LAN_COMM_ERR_BUS_BUSY,
    LAN_COMM_ERR_INVALID_STATE,
    LAN_COMM_ERR_DMA_ALIGN,
    LAN_COMM_ERR_NO_DATA,
    LAN_COMM_ERR_INVALID_HEADER
} lan_comm_status_t;
// STRUCTURES

/**
 * @brief SPI Slave Configuration
 */
typedef struct {
    // GPIO pins
    int gpio_sck;
    int gpio_cs;
    int gpio_io0;
    int gpio_io1;
    int gpio_data_ready;    // GPIO8 output, -1 to disable
    
    // SPI settings
    uint8_t mode;           // SPI mode 0-3
    spi_host_device_t host_id;
    int dma_channel;
    
    // Buffer sizes (16KB to accommodate large config JSON payloads)
    size_t rx_buffer_size;
    size_t tx_buffer_size;
    
    // Features
    bool auto_signal_data_ready;  // Auto-pulse GPIO on TX load
} lan_comm_config_t;

/**
 * @brief Default configuration macro
 */
#define LAN_COMM_CONFIG_DEFAULT() { \
    .gpio_sck = 12, \
    .gpio_cs = 10, \
    .gpio_io0 = 11, \
    .gpio_io1 = 13, \
    .gpio_data_ready = 8, \
    .mode = 0, \
    .host_id = SPI2_HOST, \
    .dma_channel = SPI_DMA_CH_AUTO, \
    .rx_buffer_size = LAN_COMM_DEFAULT_RX_BUFFER, \
    .tx_buffer_size = LAN_COMM_DEFAULT_TX_BUFFER, \
    .auto_signal_data_ready = false \
}

/**
 * @brief Opaque handle
 */
typedef struct lan_comm_handle_s *lan_comm_handle_t;

/**
 * @brief Parsed packet structure
 */
typedef struct {
    uint16_t header_type;        // CF, DT, DQ, CQ
    uint8_t *payload;            // Pointer to payload (points into RX buffer)
    uint16_t payload_length;     // Payload length (excluding header)
} lan_comm_packet_t;

/**
 * @brief Frame callback for parsed frames
 * Called for each frame found in DMA buffer
 */
typedef void (*lan_comm_frame_callback_t)(const lan_comm_packet_t *packet, void *user_arg);

// PUBLIC API
/**
 * @brief Initialize SPI Slave with DMA buffering
 * 
 * @param config Configuration structure
 * @param[out] handle Output handle
 * @return LAN_COMM_OK on success
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t *config, lan_comm_handle_t *handle);

/**
 * @brief Deinitialize SPI Slave
 */
lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle);

/**
 * @brief Queue receive transaction (pre-queue for next master TX)
 * Must be called before master transmits
 */
lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle);

/**
 * @brief Get received packet with DMA buffer parsing
 * 
 * Parses DMA buffer, skips 0x00 padding, extracts first valid frame
 * 
 * @param handle Handle
 * @param[out] packet Parsed packet structure
 * @param timeout_ms Timeout in milliseconds
 * @return LAN_COMM_OK on success
 */
lan_comm_status_t lan_comm_get_received_packet(lan_comm_handle_t handle, 
                                                 lan_comm_packet_t *packet, 
                                                 uint32_t timeout_ms);

/**
 * @brief Parse DMA buffer and call callback for each frame
 * 
 * Advanced API for processing multiple frames in one buffer
 * 
 * @param handle Handle
 * @param buffer DMA buffer to parse
 * @param length Buffer length
 * @param callback Callback for each valid frame
 * @param user_arg User argument passed to callback
 * @return Number of frames parsed
 */
uint32_t lan_comm_parse_dma_buffer(lan_comm_handle_t handle,
                                     const uint8_t *buffer,
                                     size_t length,
                                     lan_comm_frame_callback_t callback,
                                     void *user_arg);

/**
 * @brief Load TX data and queue transaction
 * 
 * @param handle Handle
 * @param data_to_send Data to send (complete frame with header)
 * @param length Data length
 * @return LAN_COMM_OK on success
 */
lan_comm_status_t lan_comm_load_tx_data(lan_comm_handle_t handle, 
                                         const uint8_t *data_to_send, 
                                         uint16_t length);

/**
 * @brief Signal data-ready to master (GPIO pulse)
 * 
 * Sends LOW → delay → HIGH pulse on data_ready GPIO
 * Master detects rising edge with ISR (<5ms response)
 */
lan_comm_status_t lan_comm_signal_data_ready(lan_comm_handle_t handle);

/**
 * @brief Get last error code
 */
lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle);

/**
 * @brief Get statistics
 */
lan_comm_status_t lan_comm_get_statistics(lan_comm_handle_t handle, 
                                           uint32_t *packets_received, 
                                           uint32_t *errors);

/**
 * @brief Clear statistics
 */
lan_comm_status_t lan_comm_clear_statistics(lan_comm_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // LAN_COMM_H
