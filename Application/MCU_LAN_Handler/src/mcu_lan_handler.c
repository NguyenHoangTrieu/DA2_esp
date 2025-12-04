/**
 * @file mcu_lan_handler.c
 * @brief MCU LAN Handler - WAN Side (SPI Slave)
 *
 * Implements Diagram 2: SPI Driver & Communication Logic
 * - Initialize SPI Slave and wait for handshake
 * - Periodic RTC and Internet status updates
 * - Forward data between LAN MCU and Server
 * - Handle config and FOTA distribution
 */
#include "mcu_lan_handler.h"
#include "config_handler.h"
#include "fota_handler.h"
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
#define RTC_UPDATE_INTERVAL_MS 1000
#define DOWNLINK_QUEUE_SIZE 20
#define MAX_RETRY_COUNT 3
#define ACK_WAIT_TIMEOUT_MS 500
#define MAX_DOWNLINK_PAYLOAD_SIZE 256

// ===== Downlink Queue Item =====
typedef struct {
  handler_id_t target_id;
  uint8_t data[MAX_DOWNLINK_PAYLOAD_SIZE];
  uint16_t length;
} downlink_item_t;

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
static QueueHandle_t g_downlink_queue = NULL;
static SemaphoreHandle_t g_config_mutex = NULL;
static bool g_handler_running = false;

static internet_status_t g_internet_status = INTERNET_STATUS_OFFLINE;
static config_cache_t g_config_cache = {0};

// External reference
extern config_server_type_t g_server_type;

// ===== Forward Declarations =====
static void mcu_lan_handler_task(void *pvParameters);
static esp_err_t perform_handshake_slave(void);
static void send_rtc_response(void);
static void send_ack_to_lan(ack_type_t ack_type, uint8_t internet_flag);
static void handle_data_from_lan(const uint8_t *payload, uint16_t length);
static void handle_config_request(void);
static void send_downlink_to_lan(const downlink_item_t *item);
static void get_rtc_string(char *buffer);
static const char *handler_id_to_string(handler_id_t id);
static esp_err_t wait_for_fota_handshake_ack(uint32_t timeout_ms);

// ===== Public API =====

esp_err_t mcu_lan_handler_start(void) {
  if (g_handler_running) {
    ESP_LOGW(TAG, "Handler already running");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Starting MCU LAN Handler (SPI Slave - WAN Side)");

  // Initialize LAN communication (SPI Slave)
  lan_comm_config_t lan_config = {.gpio_sck = 12,
                                  .gpio_cs = 10,
                                  .gpio_io0 = 11,
                                  .gpio_io1 = 13,
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
    ESP_LOGE(TAG, "Failed to initialize LAN comm (SPI Slave): %d", status);
    return ESP_FAIL;
  }

  // Create downlink queue
  g_downlink_queue = xQueueCreate(DOWNLINK_QUEUE_SIZE, sizeof(downlink_item_t));
  if (g_downlink_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create downlink queue");
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  // Create config mutex
  g_config_mutex = xSemaphoreCreateMutex();

  // Create handler task
  BaseType_t ret =
      xTaskCreate(mcu_lan_handler_task, "mcu_lan_task", MCU_LAN_TASK_STACK_SIZE,
                  NULL, MCU_LAN_TASK_PRIORITY, &g_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    vQueueDelete(g_downlink_queue);
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  g_handler_running = true;
  ESP_LOGI(TAG, "MCU LAN Handler started successfully");
  return ESP_OK;
}

esp_err_t mcu_lan_handler_stop(void) {
  if (!g_handler_running)
    return ESP_OK;

  ESP_LOGI(TAG, "Stopping MCU LAN Handler");
  g_handler_running = false;

  if (g_task_handle != NULL) {
    vTaskDelete(g_task_handle);
    g_task_handle = NULL;
  }

  if (g_downlink_queue != NULL) {
    vQueueDelete(g_downlink_queue);
    g_downlink_queue = NULL;
  }

  if (g_config_mutex != NULL) {
    vSemaphoreDelete(g_config_mutex);
    g_config_mutex = NULL;
  }

  if (g_lan_handle != NULL) {
    lan_comm_deinit(g_lan_handle);
    g_lan_handle = NULL;
  }

  return ESP_OK;
}

static esp_err_t wait_for_fota_handshake_ack(uint32_t timeout_ms) {
  TickType_t start = xTaskGetTickCount();

  while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
    lan_comm_queue_receive(g_lan_handle);

    lan_comm_packet_t packet;
    lan_comm_status_t status =
        lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

    if (status == LAN_COMM_OK && packet.payload_length >= 2) {
      if (packet.payload[0] == FRAME_TYPE_ACK &&
          packet.payload[1] == ACK_TYPE_HANDSHAKE) {
        ESP_LOGI(TAG, "FOTA handshake ACK received from LAN MCU");
        return ESP_OK;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGW(TAG, "FOTA handshake ACK timeout");
  return ESP_FAIL;
}

void mcu_lan_handler_set_internet_status(internet_status_t status) {
  if (g_internet_status != status) {
    g_internet_status = status;
    ESP_LOGI(TAG, "Internet status: %s",
             status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");
  }
}

bool mcu_lan_enqueue_downlink(handler_id_t target_id, uint8_t *data,
                              uint16_t len) {
  if (g_downlink_queue == NULL || data == NULL || len == 0) {
    ESP_LOGE(TAG, "Invalid downlink parameters");
    return false;
  }

  if (len > MAX_DOWNLINK_PAYLOAD_SIZE) {
    ESP_LOGE(TAG, "Downlink data too large");
    return false;
  }

  downlink_item_t item;
  item.target_id = target_id;
  item.length = len;
  memcpy(item.data, data, len);

  if (xQueueSend(g_downlink_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Downlink queue full");
    return false;
  }

  ESP_LOGD(TAG, "Downlink queued for handler %d (%u bytes)", target_id, len);
  return true;
}

void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length,
                                   bool is_fota) {
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (config_data && length > 0 &&
        length <= sizeof(g_config_cache.config_data)) {
      memcpy(g_config_cache.config_data, config_data, length);
      g_config_cache.config_length = length;
      g_config_cache.has_config = true;
      g_config_cache.is_fota = is_fota;
    }
    xSemaphoreGive(g_config_mutex);
  }
}

bool server_handler_enqueue_uplink(uint8_t *data, uint16_t len) {
  if (data == NULL || len == 0)
    return false;

  // Forward to MQTT/HTTP based on config
  switch (g_server_type) {
  case CONFIG_SERVERTYPE_MQTT:
    return mqtt_enqueue_telemetry(data, len);
  case CONFIG_SERVERTYPE_HTTP:
    ESP_LOGW(TAG, "HTTP not implemented");
    return false;
  default:
    return mqtt_enqueue_telemetry(data, len);
  }
}

// ===== Main Task (Diagram 2 Implementation) =====

static void mcu_lan_handler_task(void *pvParameters) {
  ESP_LOGI(TAG, "MCU LAN Handler task started");

  // ========================================
  // PHASE 1: SPI Driver Init & Handshake
  // ========================================
  ESP_LOGI(TAG, "Phase 1: Waiting for handshake from LAN MCU");

  while (g_handler_running) {
    if (perform_handshake_slave() == ESP_OK) {
      ESP_LOGI(TAG, "Handshake complete, entering Data Reception Loop");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // ========================================
  // PHASE 2: Data Reception Loop
  // ========================================
  ESP_LOGI(TAG, "Phase 2: Data Reception Loop");
  TickType_t last_rtc_update = xTaskGetTickCount();
  downlink_item_t downlink_item;

  while (g_handler_running) {
    TickType_t now = xTaskGetTickCount();

    // ===== Branch A: RTC & Internet Update Period =====
    if ((now - last_rtc_update) >= pdMS_TO_TICKS(RTC_UPDATE_INTERVAL_MS)) {
      // Preload RTC response into SPI buffer
      send_rtc_response();
      last_rtc_update = now;
    }

    // Queue receive transaction
    lan_comm_queue_receive(g_lan_handle);

    // ===== Check Received Data =====
    lan_comm_packet_t packet;
    lan_comm_status_t status =
        lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

    if (status == LAN_COMM_OK && packet.payload_length >= 2) {
      uint8_t prefix[2] = {packet.payload[0], packet.payload[1]};

      // ===== Branch: RTC Request from LAN =====
      if (prefix[0] == 'R' && prefix[1] == 'T') {
        ESP_LOGD(TAG, "RTC request received");
        send_rtc_response();
      }
      // ===== Branch: Data from MCU LAN =====
      else if (prefix[0] == 'D' && prefix[1] == 'T') {
        ESP_LOGI(TAG, "Data packet received from LAN");
        handle_data_from_lan(packet.payload, packet.payload_length);
      }
      // ===== Branch: Handshake ACK =====
      else if (packet.payload[0] == FRAME_TYPE_ACK &&
               packet.payload[1] == ACK_TYPE_HANDSHAKE) {
        // Already in data mode, respond with ACK
        send_ack_to_lan(ACK_TYPE_HANDSHAKE, 0);
      }
      // ===== Branch: Config Request =====
      else if (prefix[0] == 'C' && prefix[1] == 'F') {
        ESP_LOGI(TAG, "Config request received");
        handle_config_request();
      }
    }

    // ===== Branch: From Server Handler Task - Downlink to LAN =====
    if (xQueueReceive(g_downlink_queue, &downlink_item, 0) == pdTRUE) {
      ESP_LOGI(TAG, "Sending downlink to handler %d (%u bytes)",
               downlink_item.target_id, downlink_item.length);
      send_downlink_to_lan(&downlink_item);
    }

    // ===== Branch: From Config Task =====
    if (xSemaphoreTake(g_config_mutex, 0) == pdTRUE) {
      if (g_config_cache.has_config) {
        if (g_config_cache.is_fota) {
          // FOTA: Send CFFW command and start update sequence
          ESP_LOGI(TAG, "FOTA config detected, initiating firmware update");
          if (wait_for_fota_handshake_ack(5000) == ESP_OK) {
            ESP_LOGI(TAG, "FOTA pre-handshake completed, ready for FW update");
            fota_handler_task_start();
          } else {
            ESP_LOGW(TAG, "FOTA pre-handshake failed or timed out");
          }
        }
        // Config will be sent on next request
      }
      xSemaphoreGive(g_config_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  ESP_LOGI(TAG, "MCU LAN Handler task exiting");
  vTaskDelete(NULL);
}

// ===== Handshake Implementation (Slave Side) =====
static esp_err_t perform_handshake_slave(void) {
  lan_comm_queue_receive(g_lan_handle);

  lan_comm_packet_t packet;
  lan_comm_status_t status =
      lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

  if (status == LAN_COMM_OK && packet.payload_length >= 2) {
    if (packet.payload[0] == FRAME_TYPE_ACK &&
        packet.payload[1] == ACK_TYPE_HANDSHAKE) {
      // Received handshake ACK from LAN MCU, send ACK back
      send_ack_to_lan(ACK_TYPE_HANDSHAKE, 0);
      return ESP_OK;
    }
  }
  return ESP_FAIL;
}

// ===== Send RTC Response =====
static void send_rtc_response(void) {
  rtc_config_response_t response;

  // Set prefix "RT"
  response.prefix[0] = 'R';
  response.prefix[1] = 'T';

  // Get current RTC time
  get_rtc_string(response.rtc_string);

  // Set network status
  response.network_status =
      (g_internet_status == INTERNET_STATUS_ONLINE) ? 1 : 0;

  // Load into TX buffer
  lan_comm_load_tx_data(g_lan_handle, (uint8_t *)&response, sizeof(response));

  ESP_LOGD(TAG, "RTC response loaded: %s, Net: %d", response.rtc_string,
           response.network_status);
}

// ===== Send ACK to LAN =====
static void send_ack_to_lan(ack_type_t ack_type, uint8_t internet_flag) {
  uint8_t ack[3] = {FRAME_TYPE_ACK, ack_type, internet_flag};
  lan_comm_load_tx_data(g_lan_handle, ack, sizeof(ack));
  ESP_LOGD(TAG, "ACK sent: type=0x%02X, inet=0x%02X", ack_type, internet_flag);
}

// ===== Handle Data from LAN MCU =====
static void handle_data_from_lan(const uint8_t *payload, uint16_t length) {
  if (payload == NULL || length < DATA_PACKET_HEADER_SIZE) {
    ESP_LOGE(TAG, "Invalid data packet");
    return;
  }

  // Parse data_packet_t: [DT][handler_type(3)][length(2)][data]
  uint8_t handler_type[4] = {payload[2], payload[3], payload[4], '\0'};
  uint16_t data_length = (payload[5] << 8) | payload[6];
  const uint8_t *data = &payload[DATA_PACKET_HEADER_SIZE];

  ESP_LOGI(TAG, "Data from handler %s (%u bytes)", handler_type, data_length);

  // Send ACK with internet status
  if (g_internet_status == INTERNET_STATUS_ONLINE) {
    send_ack_to_lan(ACK_TYPE_RECEIVED_OK, ACK_TYPE_INTERNET_OK);
    // Forward to server handler
    server_handler_enqueue_uplink((uint8_t *)data, data_length);
  } else {
    send_ack_to_lan(ACK_TYPE_RECEIVED_OK, ACK_TYPE_NO_INTERNET);
    // LAN MCU will save to SD card
  }
}

// ===== Handle Config Request =====
static void handle_config_request(void) {
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (g_config_cache.has_config) {
      // Build config_data_t: [CF][length(2)][config_data]
      uint8_t config_packet[260];
      config_packet[0] = 'C';
      config_packet[1] = 'F';
      config_packet[2] = (g_config_cache.config_length >> 8) & 0xFF;
      config_packet[3] = g_config_cache.config_length & 0xFF;
      memcpy(&config_packet[4], g_config_cache.config_data,
             g_config_cache.config_length);

      lan_comm_load_tx_data(g_lan_handle, config_packet,
                            4 + g_config_cache.config_length);

      g_config_cache.has_config = false; // Clear after sending
    }
    xSemaphoreGive(g_config_mutex);
  }
}

// ===== Send Downlink to LAN with Retry =====
static void send_downlink_to_lan(const downlink_item_t *item) {
  // Build data_packet_t: [DT][handler_type(3)][length(2)][payload]
  uint16_t packet_size = DATA_PACKET_HEADER_SIZE + item->length;
  uint8_t packet[packet_size];

  packet[0] = 'D';
  packet[1] = 'T';

  const char *type_str = handler_id_to_string(item->target_id);
  memcpy(&packet[2], type_str, 3);

  packet[5] = (item->length >> 8) & 0xFF;
  packet[6] = item->length & 0xFF;

  memcpy(&packet[DATA_PACKET_HEADER_SIZE], item->data, item->length);

  // Send with retry
  for (int retry = 0; retry < MAX_RETRY_COUNT; retry++) {
    lan_comm_load_tx_data(g_lan_handle, packet, packet_size);

    // Wait for ACK from LAN MCU
    vTaskDelay(pdMS_TO_TICKS(ACK_WAIT_TIMEOUT_MS));

    lan_comm_queue_receive(g_lan_handle);
    lan_comm_packet_t ack_packet;
    lan_comm_status_t status = lan_comm_get_received_packet(
        g_lan_handle, &ack_packet, ACK_WAIT_TIMEOUT_MS);

    if (status == LAN_COMM_OK && ack_packet.payload[0] == FRAME_TYPE_ACK &&
        ack_packet.payload[1] == ACK_TYPE_RECEIVED_OK) {
      ESP_LOGD(TAG, "Downlink ACK received");
      return;
    }

    ESP_LOGW(TAG, "Downlink ACK timeout, retry %d/%d", retry + 1,
             MAX_RETRY_COUNT);
  }

  ESP_LOGE(TAG, "Downlink send failed after max retries");
}

// ===== Get RTC String =====
static void get_rtc_string(char *buffer) {
  time_t now = time(NULL);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int year = timeinfo.tm_year + 1900;
  if (year > 9999) {
    year = 9999;
  } else if (year < 0) {
    year = 0;
  }

  /* Format into a large temporary buffer to avoid -Wformat-truncation */
  char tmp[64];

  // "dd/mm/yyyy-hh:mm:ss" = 19 chars
  snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d-%02d:%02d:%02d", timeinfo.tm_mday,
           timeinfo.tm_mon + 1, year, timeinfo.tm_hour, timeinfo.tm_min,
           timeinfo.tm_sec);

  /* Copy exactly 19 characters into the 20-byte field and terminate */
  memcpy(buffer, tmp, 19);
  buffer[19] = '\0';
}

// ===== Helper =====
static const char *handler_id_to_string(handler_id_t id) {
  switch (id) {
  case HANDLER_CAN:
    return "CAN";
  case HANDLER_LORA:
    return "LOR";
  case HANDLER_ZIGBEE:
    return "ZIG";
  default:
    return "UNK";
  }
}
