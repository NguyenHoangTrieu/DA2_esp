/**
 * @file mcu_lan_handler.h
 * @brief MCU LAN Handler - WAN Side (QSPI Slave)
 */

#ifndef MCU_LAN_HANDLER_H
#define MCU_LAN_HANDLER_H

#include "esp_err.h"
#include "frame_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

// ===== Firmware Version =====
// Update these values for new releases
#define WAN_FW_VERSION_MAJOR 1
#define WAN_FW_VERSION_MINOR 1
#define WAN_FW_VERSION_PATCH 1
#define WAN_FW_VERSION_BUILD 2

#define WAN_FW_VERSION                                                         \
  FW_VERSION_MAKE(WAN_FW_VERSION_MAJOR, WAN_FW_VERSION_MINOR,                  \
                  WAN_FW_VERSION_PATCH, WAN_FW_VERSION_BUILD)

// ===== Configuration =====
typedef enum {
  CONFIG_REQ_NONE = 0,
  CONFIG_REQ_LAN_CONFIG, // Request LAN config for CFSC command
  CONFIG_REQ_RTC,        // Request RTC only
  CONFIG_REQ_INTERNET    // Request internet status only
} config_request_type_t;

typedef struct {
  config_request_type_t type;
  uint8_t *response_buffer;
  uint16_t *response_len;
  uint16_t buffer_size;
  SemaphoreHandle_t completion_sem;
  esp_err_t result;
} config_request_t;

// ===== Command Source (for ACK/response routing) =====
typedef enum {
  CMD_SOURCE_MQTT    = 0,  // From MQTT server → forward response to server
  CMD_SOURCE_UART    = 1,  // From UART PC App → forward response to UART
  CMD_SOURCE_USB     = 2,  // From USB Serial JTAG → forward response to USB
  CMD_SOURCE_UNKNOWN = 0xFF
} command_source_t;

// ===== Public API =====

/**
 * @brief Start MCU LAN handler (Priority 6 uplink task + downlink queue)
 */
esp_err_t mcu_lan_handler_start(void);

/**
 * @brief Stop MCU LAN handler
 */
esp_err_t mcu_lan_handler_stop(void);

/**
 * @brief Set internet connection status (called by MQTT/HTTP handler)
 */
void mcu_lan_handler_set_internet_status(internet_status_t status);

/**
 * @brief Enqueue downlink data from Server to LAN MCU
 */
bool mcu_lan_enqueue_downlink(handler_id_t target_id, uint8_t *data,
                              uint16_t len);

/**
 * @brief Update config data to send to LAN MCU
 * @param config_data Config data buffer
 * @param length Config length
 * @param is_fota True if FOTA command
 * @param source Command source (CMD_SOURCE_UART / CMD_SOURCE_USB / CMD_SOURCE_MQTT)
 */
void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length,
                                   bool is_fota, command_source_t source);

/**
 * @brief Get last config command source (for response routing)
 * @return command_source_t: CMD_SOURCE_UART / CMD_SOURCE_USB / CMD_SOURCE_MQTT
 */
command_source_t mcu_lan_handler_get_config_source(void);

/**
 * @brief Enqueue uplink data to server (MQTT/HTTP)
 */
bool server_handler_enqueue_uplink(const uint8_t *data, uint16_t len);

/**
 * @brief Request LAN config (async, called from UART/USB task)
 */
esp_err_t mcu_lan_handler_request_config_async(uint8_t *buffer,
                                               uint16_t *out_len,
                                               uint16_t max_len,
                                               uint32_t timeout_ms);

/**
 * @brief Get current internet status (thread-safe)
 */
internet_status_t mcu_lan_handler_get_internet_status(void);

/**
 * @brief Get RTC time string (thread-safe, cached)
 */
esp_err_t mcu_lan_handler_get_rtc(char *buffer);

/**
 * @brief Get cached LAN MCU firmware version
 */
uint32_t mcu_lan_handler_get_lan_fw_version(void);

#endif // MCU_LAN_HANDLER_H
