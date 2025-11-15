/**
 * @file lan_comm.h
 * @brief LAN MCU Communication Library (QSPI Master)
 * 
 * This library provides QSPI master functionality for communication
 * between LAN MCU (Master) and WAN MCU (Slave) using ESP-IDF SPI driver.
 */

#ifndef LAN_COMM_H
#define LAN_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Protocol headers
 */
#define LAN_COMM_HEADER_CF 0x4346  // "CF" - Command Frame
#define LAN_COMM_HEADER_DT 0x4454  // "DT" - Data Frame
#define LAN_COMM_HEADER_SIZE 2

/**
 * @brief Default configuration values
 */
#define LAN_COMM_DEFAULT_CLOCK_HZ (10 * 1000 * 1000)  // 10 MHz
#define LAN_COMM_DEFAULT_QUEUE_SIZE 7
#define LAN_COMM_MAX_TRANSFER_SIZE 8192  // SPI DMA limitation
#define LAN_COMM_TIMEOUT_MS 1000

/**
 * @brief Status codes
 */
typedef enum {
    LAN_COMM_OK = 0,
    LAN_COMM_ERR_INVALID_ARG,
    LAN_COMM_ERR_TIMEOUT,
    LAN_COMM_ERR_INVALID_STATE,
    LAN_COMM_ERR_NO_MEM,
    LAN_COMM_ERR_INVALID_HEADER,
    LAN_COMM_ERR_BUS_BUSY,
    LAN_COMM_ERR_DMA_FAILURE,
    LAN_COMM_ERR_NOT_INITIALIZED
} lan_comm_status_t;

/**
 * @brief Transfer completion callback type
 * 
 * @param status Transfer status
 * @param user_data User-provided context data
 */
typedef void (*lan_comm_transfer_cb_t)(lan_comm_status_t status, void* user_data);

/**
 * @brief Error callback type
 * 
 * @param error Error code
 * @param context Error context string
 * @param user_data User-provided context data
 */
typedef void (*lan_comm_error_cb_t)(lan_comm_status_t error, const char* context, void* user_data);

/**
 * @brief Configuration structure
 */
typedef struct {
    // GPIO pins
    int gpio_sck;           // SPI Clock
    int gpio_cs;            // Chip Select (software controlled)
    int gpio_io0;           // MOSI / IO0
    int gpio_io1;           // MISO / IO1
    int gpio_io2;           // WP / IO2 (for Quad mode)
    int gpio_io3;           // HD / IO3 (for Quad mode)
    
    // SPI configuration
    uint32_t clock_speed_hz;    // SPI clock frequency
    uint8_t mode;               // SPI mode (0-3)
    spi_host_device_t host_id;  // SPI peripheral (SPI2_HOST or SPI3_HOST)
    
    // DMA configuration
    int dma_channel;            // DMA channel (SPI_DMA_CH_AUTO recommended)
    uint16_t queue_size;        // Transaction queue size
    
    // Callbacks
    lan_comm_transfer_cb_t transfer_callback;  // Transfer complete callback (for non-blocking)
    lan_comm_error_cb_t error_callback;        // Error callback
    void* user_data;                           // User context for callbacks
    
    // Options
    bool enable_quad_mode;      // Enable QSPI (4-bit) mode
} lan_comm_config_t;

/**
 * @brief Handle structure (opaque) (for hiding implementation details, user cant see internal members - safety purpose)
 */
typedef struct lan_comm_handle_s* lan_comm_handle_t;

/**
 * @brief Initialize LAN communication library
 * 
 * @param config Configuration structure
 * @param handle Output handle pointer
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t* config, lan_comm_handle_t* handle);

/**
 * @brief Deinitialize LAN communication library
 * 
 * @param handle Handle to deinitialize
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle);

/**
 * @brief Send command packet to WAN MCU
 * 
 * Automatically prepends CF header to payload
 * 
 * @param handle Communication handle
 * @param command_payload Command data
 * @param length Payload length (excluding header)
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_send_command(lan_comm_handle_t handle, 
                                        const uint8_t* command_payload, 
                                        uint16_t length);

/**
 * @brief Send data packet to WAN MCU
 * 
 * Automatically prepends DT header to payload
 * 
 * @param handle Communication handle
 * @param data_payload Data to send
 * @param length Payload length (excluding header)
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_send_data(lan_comm_handle_t handle, 
                                     const uint8_t* data_payload, 
                                     uint16_t length);

/**
 * @brief Request data from WAN MCU
 * 
 * Performs a read transaction (master clocks, slave sends data)
 * 
 * @param handle Communication handle
 * @param rx_buffer Buffer to receive data
 * @param length_to_read Number of bytes to read
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_request_data(lan_comm_handle_t handle, 
                                        uint8_t* rx_buffer, 
                                        uint16_t length_to_read);

/**
 * @brief Set blocking/non-blocking mode
 * 
 * @param handle Communication handle
 * @param blocking true for blocking, false for non-blocking
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_set_blocking_mode(lan_comm_handle_t handle, bool blocking);

/**
 * @brief Get last error status
 * 
 * @param handle Communication handle
 * @return lan_comm_status_t Last error code
 */
lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle);

/**
 * @brief Register error callback
 * 
 * @param handle Communication handle
 * @param callback Error callback function
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_register_error_callback(lan_comm_handle_t handle, 
                                                   lan_comm_error_cb_t callback);

/**
 * @brief Get error count
 * 
 * @param handle Communication handle
 * @return uint32_t Number of errors occurred
 */
uint32_t lan_comm_get_error_count(lan_comm_handle_t handle);

/**
 * @brief Clear error count
 * 
 * @param handle Communication handle
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_clear_error_count(lan_comm_handle_t handle);

/**
 * @brief Get header size constant
 * 
 * @return uint16_t Header size in bytes
 */
static inline uint16_t lan_comm_get_header_size(void) {
    return LAN_COMM_HEADER_SIZE;
}

#ifdef __cplusplus
}
#endif

#endif // LAN_COMM_H
