/**
 * @file mcu_lan_handler.c
 * @brief MCU LAN Communication Handler Implementation (SPI Slave - WAN MCU)
 */

#include "mcu_lan_handler.h"
#include "config_handler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lan_comm.h"
#include "mqtt_handler.h"
#include <string.h>
#include <time.h>

static const char *TAG = "MCU_LAN";

// ===== Configuration =====
#define MCU_LAN_TASK_STACK_SIZE 4096
#define MCU_LAN_TASK_PRIORITY 5
#define RX_TIMEOUT_MS 100

// ===== Protocol Commands =====
#define CMD_HANDSHAKE_ACK 0x01
#define CMD_REQUEST_RTC_CONFIG 0x02
#define CMD_DATA_PACKET 0x03

// ===== ACK Types =====
#define ACK_RECEIVED_INTERNET_OK 0x01
#define ACK_RECEIVED_NO_INTERNET 0x02

// ===== RTC Structure =====
typedef struct {
  uint8_t day;
  uint8_t month;
  uint16_t year;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} rtc_time_t;

// ===== Config Cache =====
typedef struct {
  uint8_t config_data[256];
  uint16_t config_length;
  bool has_config;
  bool is_fota;
} config_cache_t;

// ===== Global Variables =====
static lan_comm_handle_t g_lan_handle = NULL;
static TaskHandle_t g_task_handle = NULL;
static bool g_handler_running = false;
static internet_status_t g_internet_status = INTERNET_STATUS_OFFLINE;
static config_cache_t g_config_cache = {0};
static SemaphoreHandle_t g_config_mutex = NULL;

// ===== Forward Declarations =====
static void mcu_lan_handler_task(void *pvParameters);
static esp_err_t perform_handshake_slave(void);
static esp_err_t handle_received_data(const uint8_t *payload, uint16_t length);
static esp_err_t handle_rtc_config_request(void);
static void send_ack_to_lan(uint8_t ack_type);
static void get_rtc_time(rtc_time_t *rtc);
static void forward_to_server(const uint8_t *data, uint16_t length);

// ===== Public API =====

esp_err_t mcu_lan_handler_start(void) {
  if (g_handler_running) {
    ESP_LOGW(TAG, "Handler already running");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Starting MCU LAN Handler (SPI Slave)");

  // Initialize LAN communication
  lan_comm_config_t lan_config = {.gpio_sck = 12,
                                  .gpio_cs = 10,
                                  .gpio_io0 = 11, // MOSI
                                  .gpio_io1 = 13, // MISO
                                  .gpio_io2 = -1,
                                  .gpio_io3 = -1,
                                  .mode = 0,
                                  .host_id = SPI2_HOST,
                                  .dma_channel = SPI_DMA_CH_AUTO,
                                  .rx_buffer_size = 4096,
                                  .tx_buffer_size = 4096,
                                  .enable_quad_mode = false};

  lan_comm_status_t status = lan_comm_init(&lan_config, &g_lan_handle);
  if (status != LAN_COMM_OK) {
    ESP_LOGE(TAG, "Failed to initialize LAN comm: %d", status);
    return ESP_FAIL;
  }

  // TODO: Initialize RTC hardware
  ESP_LOGI(TAG, "TODO: Initialize RTC hardware (DS1307/DS3231)");

  // Create handler task
  BaseType_t ret =
      xTaskCreate(mcu_lan_handler_task, "mcu_lan_task", MCU_LAN_TASK_STACK_SIZE,
                  NULL, MCU_LAN_TASK_PRIORITY, &g_task_handle);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  g_handler_running = true;
  ESP_LOGI(TAG, "MCU LAN Handler started successfully");
  return ESP_OK;
}

esp_err_t mcu_lan_handler_stop(void) {
  if (!g_handler_running) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Stopping MCU LAN Handler");
  g_handler_running = false;

  if (g_task_handle != NULL) {
    vTaskDelete(g_task_handle);
    g_task_handle = NULL;
  }

  if (g_lan_handle != NULL) {
    lan_comm_deinit(g_lan_handle);
    g_lan_handle = NULL;
  }

  ESP_LOGI(TAG, "MCU LAN Handler stopped");
  return ESP_OK;
}

void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length,
                                   bool is_fota) {
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (config_data == NULL || length == 0 ||
        length > sizeof(g_config_cache.config_data)) {
      ESP_LOGE(TAG, "Invalid config data");
      xSemaphoreGive(g_config_mutex);
      return;
    }

    memset(&g_config_cache, 0, sizeof(g_config_cache));
    memcpy(g_config_cache.config_data, config_data, length);
    g_config_cache.config_length = length;
    g_config_cache.has_config = true;
    g_config_cache.is_fota = is_fota;

    ESP_LOGI(TAG, "Config updated: %.*s (FOTA: %s)", length, config_data,
             is_fota ? "Yes" : "No");
    xSemaphoreGive(g_config_mutex);
  }
}

void mcu_lan_handler_set_internet_status(internet_status_t status) {
  if (g_internet_status != status) {
    g_internet_status = status;
    ESP_LOGI(TAG, "Internet status: %s",
             status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");
  }
}

// ===== Main Task =====

static void mcu_lan_handler_task(void *pvParameters) {
  ESP_LOGI(TAG, "MCU LAN Handler task started");

  // Phase 1: Handshake
  ESP_LOGI(TAG, "Phase 1: Waiting for handshake from LAN MCU");
  while (g_handler_running) {
    if (perform_handshake_slave() == ESP_OK) {
      ESP_LOGI(TAG, "Handshake successful, entering Data Mode");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // Phase 2: Data Reception Loop
  ESP_LOGI(TAG, "Phase 2: Data Reception Loop");
  while (g_handler_running) {
    // Queue receive transaction
    lan_comm_queue_receive(g_lan_handle);

    // Wait for data from LAN MCU
    lan_comm_packet_t packet;
    lan_comm_status_t status =
        lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

    if (status == LAN_COMM_OK) {
      // Check data type based on header
      if (packet.header_type == LAN_COMM_HEADER_DT) {
        // Standard telemetry data
        ESP_LOGI(TAG, "Received data packet (%d bytes)", packet.payload_length);
        handle_received_data(packet.payload, packet.payload_length);

      } else if (packet.header_type == LAN_COMM_HEADER_CF) {
        // Command received
        uint8_t cmd = packet.payload[0];

        if (cmd == CMD_HANDSHAKE_ACK) {
          ESP_LOGD(TAG, "Handshake ACK (already in data mode)");
          send_ack_to_lan(ACK_RECEIVED_INTERNET_OK);

        } else if (cmd == CMD_REQUEST_RTC_CONFIG) {
          ESP_LOGI(TAG, "Request for RTC/Config/Internet Status");
          handle_rtc_config_request();

        } else {
          ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
        }
      }
    } else if (status != LAN_COMM_ERR_TIMEOUT) {
      ESP_LOGE(TAG, "Error receiving packet: %d", status);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  ESP_LOGI(TAG, "MCU LAN Handler task exiting");
  vTaskDelete(NULL);
}

// ===== Handshake Implementation =====

static esp_err_t perform_handshake_slave(void) {
  // Queue receive transaction
  lan_comm_queue_receive(g_lan_handle);

  // Wait for handshake ACK from LAN MCU
  lan_comm_packet_t packet;
  lan_comm_status_t status =
      lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

  if (status == LAN_COMM_OK) {
    if (packet.header_type == LAN_COMM_HEADER_CF &&
        packet.payload[0] == CMD_HANDSHAKE_ACK) {
      ESP_LOGI(TAG, "Received handshake ACK from LAN MCU");

      // Send ACK back
      uint8_t ack_response[3] = {(LAN_COMM_HEADER_CF >> 8) & 0xFF,
                                 LAN_COMM_HEADER_CF & 0xFF, CMD_HANDSHAKE_ACK};

      lan_comm_load_tx_data(g_lan_handle, ack_response, sizeof(ack_response));
      ESP_LOGI(TAG, "Sent ACK back to LAN MCU");
      return ESP_OK;
    }
  }

  return ESP_FAIL;
}

// ===== Data Handling =====

static esp_err_t handle_received_data(const uint8_t *payload, uint16_t length) {
  if (payload == NULL || length < 19) { // Minimum: RTC timestamp
    return ESP_ERR_INVALID_ARG;
  }

  // Extract RTC timestamp (first 19 bytes)
  char rtc_str[20] = {0};
  memcpy(rtc_str, payload, 19);

  // Extract actual data (after RTC)
  const uint8_t *data = &payload[19];
  uint16_t data_length = length - 19;

  ESP_LOGI(TAG, "Data with RTC: %s, Data length: %d", rtc_str, data_length);

  // Check internet status
  if (g_internet_status == INTERNET_STATUS_ONLINE) {
    // Forward to server handler based on g_server_type
    forward_to_server(data, data_length);
    send_ack_to_lan(ACK_RECEIVED_INTERNET_OK);
  } else {
    ESP_LOGW(TAG, "Internet offline, cannot forward to server");
    send_ack_to_lan(ACK_RECEIVED_NO_INTERNET);
  }

  return ESP_OK;
}

// ===== Server Forwarding =====

static void forward_to_server(const uint8_t *data, uint16_t length) {
  // Use g_server_type from config_handler to determine server type
  switch (g_server_type) {
  case CONFIG_SERVERTYPE_MQTT:
    ESP_LOGI(TAG, "Forwarding to MQTT server");
    mqtt_enqueue_telemetry(data, length);
    break;

  case CONFIG_SERVERTYPE_COAP:
    ESP_LOGW(TAG, "CoAP server not implemented yet");
    // TODO: Implement CoAP forwarding
    break;

  case CONFIG_SERVERTYPE_HTTP:
    ESP_LOGW(TAG, "HTTP server not implemented yet");
    // TODO: Implement HTTP forwarding
    break;

  default:
    ESP_LOGE(TAG, "Unknown server type: %d, defaulting to MQTT", g_server_type);
    mqtt_enqueue_telemetry(data, length);
    break;
  }
}

// ===== RTC & Config Request Handler =====

static esp_err_t handle_rtc_config_request(void) {
  uint8_t response[512] = {0};
  uint16_t offset = 0;

  // Header
  response[offset++] = (LAN_COMM_HEADER_CF >> 8) & 0xFF;
  response[offset++] = LAN_COMM_HEADER_CF & 0xFF;
  response[offset++] = CMD_REQUEST_RTC_CONFIG;

  // Get current RTC time
  rtc_time_t rtc;
  get_rtc_time(&rtc);

  // Format RTC (19 bytes): dd/mm/yyyy-hh:mm:ss
  int rtc_len =
      snprintf((char *)&response[offset], 20, "%02d/%02d/%04d-%02d:%02d:%02d",
               rtc.day, rtc.month, rtc.year, rtc.hour, rtc.minute, rtc.second);

  if (rtc_len == 19) {
    offset += 19;
  } else {
    ESP_LOGE(TAG, "RTC format error");
    return ESP_FAIL;
  }

  // Separator
  response[offset++] = '-';

  // Config data
  if (g_config_cache.has_config) {
    memcpy(&response[offset], g_config_cache.config_data,
           g_config_cache.config_length);
    offset += g_config_cache.config_length;
  } else {
    // No config
    const char *no_config = "NO_CF";
    memcpy(&response[offset], no_config, 5);
    offset += 5;
  }

  // Separator
  response[offset++] = '-';

  // Internet status
  response[offset++] =
      (g_internet_status == INTERNET_STATUS_ONLINE) ? 0x01 : 0x00;

  // Load TX data
  lan_comm_load_tx_data(g_lan_handle, response, offset);

  ESP_LOGI(TAG, "Sent RTC/Config/Status response (%d bytes)", offset);
  ESP_LOGI(TAG, "  RTC: %02d/%02d/%04d-%02d:%02d:%02d", rtc.day, rtc.month,
           rtc.year, rtc.hour, rtc.minute, rtc.second);
  ESP_LOGI(TAG, "  Config: %s", g_config_cache.has_config ? "Yes" : "No");
  ESP_LOGI(TAG, "  Internet: %s",
           g_internet_status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");

  // Check if FOTA config and notify config task
  if (g_config_cache.is_fota && g_config_cache.has_config) {
    ESP_LOGI(TAG, "FOTA config detected - notifying config task");
    // TODO: Notify config task to start firmware update
    // This could be done via event group or direct function call
  }

  return ESP_OK;
}

// ===== Send ACK =====

static void send_ack_to_lan(uint8_t ack_type) {
  uint8_t ack_response[3] = {(LAN_COMM_HEADER_CF >> 8) & 0xFF,
                             LAN_COMM_HEADER_CF & 0xFF, ack_type};

  lan_comm_load_tx_data(g_lan_handle, ack_response, sizeof(ack_response));
  ESP_LOGD(TAG, "ACK sent to LAN MCU (type: 0x%02X)", ack_type);
}

// ===== RTC Functions =====

static void get_rtc_time(rtc_time_t *rtc) {
  // TODO: Read from RTC hardware (DS1307, DS3231, etc.)
  /*
   * Pseudo implementation:
   * 1. I2C read from RTC device
   * 2. Convert BCD to decimal
   * 3. Fill rtc structure
   */

  // Fallback: use system time
  time_t now = time(NULL);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  rtc->day = timeinfo.tm_mday;
  rtc->month = timeinfo.tm_mon + 1;
  rtc->year = timeinfo.tm_year + 1900;
  rtc->hour = timeinfo.tm_hour;
  rtc->minute = timeinfo.tm_min;
  rtc->second = timeinfo.tm_sec;
}
