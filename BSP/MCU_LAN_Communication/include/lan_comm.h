/**
 * @file lan_comm.h
 * @brief LAN Communication Library for WAN MCU (SPI Slave)
 */

#ifndef LAN_COMM_H
#define LAN_COMM_H

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Protocol headers
 */
#define LAN_COMM_HEADER_CF 0x4346 // "CF" - Command Frame
#define LAN_COMM_HEADER_DT 0x4454 // "DT" - Data Frame
#define LAN_COMM_HEADER_SIZE 2

/**
 * @brief Default configuration values
 */
#define LAN_COMM_DEFAULT_RX_BUFFER_SIZE 4096
#define LAN_COMM_DEFAULT_TX_BUFFER_SIZE 4096
#define LAN_COMM_TRANS_QUEUE_SIZE 3

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
  LAN_COMM_ERR_NOT_INITIALIZED,
  LAN_COMM_ERR_NO_DATA
} lan_comm_status_t;

/**
 * @brief Configuration structure
 */
typedef struct {
  // GPIO pins
  int gpio_sck; // SPI Clock
  int gpio_cs;  // Chip Select
  int gpio_io0; // MOSI / IO0
  int gpio_io1; // MISO / IO1
  int gpio_io2; // WP / IO2 (for Quad mode)
  int gpio_io3; // HD / IO3 (for Quad mode)

  // SPI configuration
  uint8_t mode;              // SPI mode (0-3)
  spi_host_device_t host_id; // SPI peripheral (SPI2_HOST or SPI3_HOST)

  // DMA configuration
  int dma_channel; // DMA channel (SPI_DMA_CH_AUTO recommended)

  // Buffer sizes
  uint16_t rx_buffer_size; // Size of RX buffer
  uint16_t tx_buffer_size; // Size of TX buffer

  // Options
  bool enable_quad_mode; // Enable QSPI (4-bit) mode
} lan_comm_config_t;

/**
 * @brief Received packet structure
 */
typedef struct {
  uint16_t header_type;    // LAN_COMM_HEADER_CF or LAN_COMM_HEADER_DT
  uint8_t *payload;        // Pointer to payload data
  uint16_t payload_length; // Payload length
} lan_comm_packet_t;

/**
 * @brief Handle structure (opaque)
 */
typedef struct lan_comm_handle_s *lan_comm_handle_t;

/**
 * @brief Initialize LAN communication library (Slave mode)
 *
 * @param config Configuration structure
 * @param handle Output handle pointer
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_init(const lan_comm_config_t *config,
                                lan_comm_handle_t *handle);

/**
 * @brief Deinitialize LAN communication library
 *
 * @param handle Handle to deinitialize
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_deinit(lan_comm_handle_t handle);

/**
 * @brief Prepare to receive data from master
 * Queue a receive transaction that will be filled when master sends data
 *
 * @param handle Communication handle
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_queue_receive(lan_comm_handle_t handle);

/**
 * @brief Check if received data is available (non-blocking)
 *
 * @param handle Communication handle
 * @param packet Output packet structure (filled if data available)
 * @param timeout_ms Timeout in milliseconds (0 for non-blocking)
 * @return lan_comm_status_t LAN_COMM_OK if data received, LAN_COMM_ERR_TIMEOUT
 * if no data
 */
lan_comm_status_t lan_comm_get_received_packet(lan_comm_handle_t handle,
                                               lan_comm_packet_t *packet,
                                               uint32_t timeout_ms);

/**
 * @brief Load data into TX buffer for master to read
 * This data will be sent automatically when master initiates a read transaction
 *
 * @param handle Communication handle
 * @param data_to_send Data to load into TX buffer
 * @param length Data length
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_load_tx_data(lan_comm_handle_t handle,
                                        const uint8_t *data_to_send,
                                        uint16_t length);

/**
 * @brief Get last error status
 *
 * @param handle Communication handle
 * @return lan_comm_status_t Last error code
 */
lan_comm_status_t lan_comm_get_last_error(lan_comm_handle_t handle);

/**
 * @brief Get statistics
 *
 * @param handle Communication handle
 * @param packets_received Output: number of packets received
 * @param errors Output: number of errors
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_get_statistics(lan_comm_handle_t handle,
                                          uint32_t *packets_received,
                                          uint32_t *errors);

/**
 * @brief Clear statistics counters
 *
 * @param handle Communication handle
 * @return lan_comm_status_t Status code
 */
lan_comm_status_t lan_comm_clear_statistics(lan_comm_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // LAN_COMM_H
