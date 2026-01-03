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
#include "DA2_esp.h"
#include "config_handler.h"
#include "pcf8563_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fota_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lan_comm.h"
#include "mqtt_handler.h"
#include "uart_handler.h"
#include "usb_handler.h"
#include <string.h>
#include <time.h>
#include "rbg_handler.h"

static const char *TAG = "MCU_LAN";

// ===== Configuration =====
#define MCU_LAN_TASK_STACK_SIZE 4096
#define MCU_LAN_TASK_PRIORITY 6
#define RX_TIMEOUT_MS 100
#define RTC_UPDATE_INTERVAL_MS 1000
#define DOWNLINK_QUEUE_SIZE 20
#define MAX_RETRY_COUNT 3
#define ACK_WAIT_TIMEOUT_MS 500
#define MAX_DOWNLINK_PAYLOAD_SIZE 1024

// ===== Downlink Queue Item =====
typedef struct {
  handler_id_t target_id;
  uint8_t data[MAX_DOWNLINK_PAYLOAD_SIZE];
  uint16_t length;
} downlink_item_t;

// ===== Config Cache =====
typedef struct {
  uint8_t config_data[512];
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
// ===== Pending Downlink =====
static downlink_item_t g_pending_downlink;
static bool g_pending_downlink_valid = false; // WAN pending data for LAN

typedef enum {
  CONFIG_REQ_STATE_IDLE = 0,
  CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ, // waiting DQ before sending CFCQ
  CONFIG_REQ_STATE_WAIT_CONFIG_RESP  // waiting CQ response from LAN
} config_req_state_t;

// ===== Config Request Storage (Heap-allocated, not stack) =====
typedef struct {
  config_request_t request;           // Full request struct
  SemaphoreHandle_t completion_sem;  // Separate semaphore
  esp_err_t result;                  // Result status
} config_request_async_t;

static config_req_state_t g_config_req_state = CONFIG_REQ_STATE_IDLE;
static config_request_t *g_active_config_request_ptr = NULL;
static bool g_active_config_request_valid = false;
static config_request_async_t *g_config_req_async = NULL;

// ===== RTC Cache (Thread-safe) =====
typedef struct {
  char rtc_string[20];
  bool valid;
  SemaphoreHandle_t mutex;
} rtc_cache_t;

static rtc_cache_t g_rtc_cache = {.valid = false};

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

// ===== GPIO Handshake Configuration =====
// NOTE: Must match GPIO pin on LAN MCU (GPIO8)
#define GPIO_DATA_READY_PIN 8

// Setup GPIO 8 as output
static esp_err_t setup_data_ready_gpio(void) {
  gpio_config_t io_conf = {.pin_bit_mask = BIT64(GPIO_DATA_READY_PIN),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure GPIO %d", GPIO_DATA_READY_PIN);
    return ret;
  }

  // Initialize to LOW
  gpio_set_level(GPIO_DATA_READY_PIN, 0);

  ESP_LOGI(TAG, "GPIO %d configured for data-ready signaling",
           GPIO_DATA_READY_PIN);
  return ESP_OK;
}

// Signal that data is ready
static void signal_data_ready(void) {
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(10)); // Short delay to ensure readiness
  gpio_set_level(GPIO_DATA_READY_PIN, 1);
  ESP_LOGI(TAG, "Data-ready signal HIGH");
}

// Clear data-ready signal
static void clear_data_ready(void) {
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  ESP_LOGI(TAG, "Data-ready signal LOW");
}

// ===== Config Request Handling =====
// ===== Public API =====
esp_err_t mcu_lan_handler_request_config_async(uint8_t *buffer,
                                               uint16_t *out_len,
                                               uint16_t max_len,
                                               uint32_t timeout_ms) {
  if (buffer == NULL || out_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(TAG, "Requesting config from LAN MCU asynchronously");

  if (g_active_config_request_valid) {
    ESP_LOGW(TAG, "Config request already in progress");
    return ESP_ERR_INVALID_STATE;
  }

  // Allocate request on HEAP (not stack!) - prevents use-after-free bug
  g_config_req_async = (config_request_async_t *)malloc(sizeof(config_request_async_t));
  if (g_config_req_async == NULL) {
    ESP_LOGE(TAG, "Failed to allocate config request");
    return ESP_ERR_NO_MEM;
  }

  // Create completion semaphore
  g_config_req_async->completion_sem = xSemaphoreCreateBinary();
  if (g_config_req_async->completion_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create completion semaphore");
    free(g_config_req_async);
    g_config_req_async = NULL;
    return ESP_ERR_NO_MEM;
  }

  // Initialize request on heap
  g_config_req_async->request.type = CONFIG_REQ_LAN_CONFIG;
  g_config_req_async->request.response_buffer = buffer;
  g_config_req_async->request.response_len = out_len;
  g_config_req_async->request.buffer_size = max_len;
  g_config_req_async->request.completion_sem = g_config_req_async->completion_sem;
  g_config_req_async->request.result = ESP_FAIL;
  g_config_req_async->result = ESP_FAIL;

  // Update globals SAFELY
  g_active_config_request_ptr = &g_config_req_async->request;
  g_active_config_request_valid = true;
  g_config_req_state = CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ;

  ESP_LOGI(TAG, "New config request from UART handler (heap-allocated at %p)", g_config_req_async);
  signal_data_ready();

  // Wait for completion
  if (xSemaphoreTake(g_config_req_async->completion_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGE(TAG, "Config request timeout");
    clear_data_ready();
    vSemaphoreDelete(g_config_req_async->completion_sem);
    free(g_config_req_async);
    g_config_req_async = NULL;
    g_active_config_request_valid = false;
    g_active_config_request_ptr = NULL;
    g_config_req_state = CONFIG_REQ_STATE_IDLE;
    return ESP_ERR_TIMEOUT;
  }

  // Get result from heap-allocated struct (safe now!)
  esp_err_t result = g_config_req_async->request.result;
  ESP_LOGI(TAG, "Config request completed with result: %s",
           esp_err_to_name(result));

  // Clean up
  vSemaphoreDelete(g_config_req_async->completion_sem);
  free(g_config_req_async);
  g_config_req_async = NULL;
  g_active_config_request_valid = false;
  g_active_config_request_ptr = NULL;
  g_config_req_state = CONFIG_REQ_STATE_IDLE;

  return result;
}

// Thread-safe getters (no SPI access)
internet_status_t mcu_lan_handler_get_internet_status(void) {
  return g_internet_status;
}

esp_err_t mcu_lan_handler_get_rtc(char *buffer) {
  if (buffer == NULL || g_rtc_cache.mutex == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_rtc_cache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (g_rtc_cache.valid) {
      memcpy(buffer, g_rtc_cache.rtc_string, 20);
      xSemaphoreGive(g_rtc_cache.mutex);
      return ESP_OK;
    }
    xSemaphoreGive(g_rtc_cache.mutex);
  }

  return ESP_ERR_NOT_FOUND;
}

esp_err_t mcu_lan_handler_start(void) {
  if (g_handler_running) {
    ESP_LOGW(TAG, "Handler already running");
    return ESP_OK;
  }

  g_rtc_cache.mutex = xSemaphoreCreateMutex();
  if (g_rtc_cache.mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create RTC mutex");
    return ESP_FAIL;
  }

  if (setup_data_ready_gpio() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to setup data-ready GPIO");
    return ESP_FAIL;
  }

  if (pcf8563_init() != ESP_OK) {
    ESP_LOGW(TAG, "PCF8563 init failed, time fallback to system");
    // Continue without PCF8563
  } else {
    // INITIAL SYNC: Read RTC time and set system time
    // After this, system time is synced with RTC
    // When WiFi connects, SNTP will update both system time and PCF8563
    struct tm rtc_timeinfo;
    if (pcf8563_read_time(&rtc_timeinfo) == ESP_OK) {
      time_t rtc_time = mktime(&rtc_timeinfo);
      struct timeval tv = {.tv_sec = rtc_time, .tv_usec = 0};
      settimeofday(&tv, NULL);
      ESP_LOGI(TAG, "System time synced from RTC: %02d/%02d/%04d-%02d:%02d:%02d",
               rtc_timeinfo.tm_mday, rtc_timeinfo.tm_mon + 1,
               rtc_timeinfo.tm_year + 1900, rtc_timeinfo.tm_hour,
               rtc_timeinfo.tm_min, rtc_timeinfo.tm_sec);
    } else {
      ESP_LOGW(TAG, "Failed to read RTC, using default system time");
    }
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
  g_handler_running = true;
  BaseType_t ret =
      xTaskCreate(mcu_lan_handler_task, "mcu_lan_task", MCU_LAN_TASK_STACK_SIZE,
                  NULL, MCU_LAN_TASK_PRIORITY, &g_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    vQueueDelete(g_downlink_queue);
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MCU LAN Handler started successfully");
  return ESP_OK;
}

esp_err_t mcu_lan_handler_stop(void) {
  if (!g_handler_running)
    return ESP_OK;

  ESP_LOGI(TAG, "Stopping MCU LAN Handler");
  pcf8563_deinit();
  g_handler_running = false;

  if (g_rtc_cache.mutex != NULL) {
    vSemaphoreDelete(g_rtc_cache.mutex);
    g_rtc_cache.mutex = NULL;
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

  ESP_LOGI(TAG, "Downlink queued for handler %d (%u bytes)", target_id, len);
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
      signal_data_ready();
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
  downlink_item_t downlink_item;

  while (g_handler_running) {
    // Always queue receive transaction to get Master's data
    lan_comm_queue_receive(g_lan_handle);

    // ===== Check Received Data =====
    lan_comm_packet_t packet;
    lan_comm_status_t status =
        lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);
    
    // Skip processing if no valid data (polling garbage, timeout, etc.)
    if (status != LAN_COMM_OK || packet.payload_length < 2) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Debug: Log all received packets to diagnose SPI communication issues
    ESP_LOGD(TAG, "SPI RX: [0]=0x%02X [1]=0x%02X len=%u", 
             packet.payload[0], packet.payload[1], packet.payload_length);

    // Process valid packet
    uint8_t prefix[2] = {packet.payload[0], packet.payload[1]};
      // ===== Branch: RTC Request from LAN =====
      if (prefix[0] == 'R' && prefix[1] == 'T') {
        ESP_LOGI(TAG, "RTC request received from LAN MCU");
        send_rtc_response();
      }
      // ===== Branch: Generic data/config request from LAN (after GPIO
      // handshake) =====
      else if (prefix[0] == 'D' && prefix[1] == 'Q') {
        ESP_LOGI(TAG, "DQ request received from LAN MCU");
        bool served = false;

        // 1) PC-side config scan: send CFCQ only after DQ
        if (g_active_config_request_valid &&
            g_config_req_state == CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ) {

          uint8_t config_request[4];
          config_request[0] = (LAN_COMM_HEADER_CF >> 8) & 0xFF;
          config_request[1] = LAN_COMM_HEADER_CF & 0xFF;
          config_request[2] = 'C';
          config_request[3] = 'Q';

          // Load CFCQ into TX only here (after DQ)
          lan_comm_load_tx_data(g_lan_handle, config_request,
                                sizeof(config_request));
          lan_comm_queue_receive(g_lan_handle);
          g_config_req_state = CONFIG_REQ_STATE_WAIT_CONFIG_RESP;
          served = true;
          ESP_LOGI(TAG, "CFCQ command loaded to TX after DQ");
        }

        // 2) Config/FOTA downlink to LAN (from WAN config cache)
        if (!served &&
            xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          if (g_config_cache.has_config) {
            ESP_LOGI(TAG, "Sending cached config/FOTA to LAN MCU");
            handle_config_request(); // only loads TX for CF payload
            served = true;
          }
          if (g_config_cache.is_fota) {
            ESP_LOGI(TAG, "FOTA config detected, initiating firmware update");
            if (wait_for_fota_handshake_ack(300000) == ESP_OK) {
              ESP_LOGI(TAG,
                       "FOTA pre-handshake completed, ready for FW update");
              server_connect_stop(g_server_type);
              led_show_blue();
              vTaskDelay(pdMS_TO_TICKS(5000));
              fota_handler_task_start();
              vTaskDelay(pdMS_TO_TICKS(
                  200000)); // Block this task until FOTA done and MCU reset
            } else {
              ESP_LOGW(TAG, "FOTA pre-handshake failed or timed out");
            }
          }
          xSemaphoreGive(g_config_mutex);
        }

        // 3) Normal downlink data (DT) to LAN
        if (!served && g_pending_downlink_valid) {
          send_downlink_to_lan(
              &g_pending_downlink); // only loads TX + waits ACK
          g_pending_downlink_valid = false;
          served = true;
        }

        if (served) {
          // Clear GPIO handshake once something has been served for this DQ
          clear_data_ready();
        } else {
          ESP_LOGW(TAG, "DQ received but nothing pending to send");
        }
      }
      // ===== Branch: Data from MCU LAN =====
      else if (prefix[0] == 'D' && prefix[1] == 'T') {
        ESP_LOGI(TAG, "Data packet received from LAN");
        handle_data_from_lan(packet.payload, packet.payload_length);
      }
      // ===== Branch: LAN config response for PC (CQ) =====
      else if (prefix[0] == 'C' && prefix[1] == 'Q') {
        if (g_active_config_request_valid &&
            g_config_req_state == CONFIG_REQ_STATE_WAIT_CONFIG_RESP &&
            packet.payload_length >= 4) {

          uint16_t config_len = (packet.payload[2] << 8) | packet.payload[3];
          if (config_len > g_active_config_request_ptr->buffer_size - 1) {
            config_len = g_active_config_request_ptr->buffer_size - 1;
          }

          // Copy config data to response buffer
          memcpy(g_active_config_request_ptr->response_buffer,
                 &packet.payload[4], config_len);
          g_active_config_request_ptr->response_buffer[config_len] = '\0';
          *g_active_config_request_ptr->response_len = config_len;

          // Update result in ORIGINAL struct (via pointer)
          g_active_config_request_ptr->result = ESP_OK;

          ESP_LOGI(TAG, "LAN config response processed (%u bytes)", config_len);

          // Signal completion semaphore BEFORE resetting
          if (g_active_config_request_ptr->completion_sem) {
            xSemaphoreGive(g_active_config_request_ptr->completion_sem);
          }

          // Reset state AFTER signaling
          g_active_config_request_valid = false;
          g_active_config_request_ptr = NULL;
          g_config_req_state = CONFIG_REQ_STATE_IDLE;

        } else {
          ESP_LOGW(TAG,
                   "Unexpected CQ packet or no active config request (len=%u)",
                   (unsigned)packet.payload_length);
        }
      }
      // ===== Branch: Handshake ACK (legacy, log only) =====
      else if (packet.payload[0] == FRAME_TYPE_ACK &&
               packet.payload[1] == ACK_TYPE_HANDSHAKE) {
        // In data mode, respond with ACK but log at debug level
        ESP_LOGD(TAG, "Handshake ACK received in data mode, responding");
        send_ack_to_lan(ACK_TYPE_HANDSHAKE, 0);
      }
      // ===== Branch: Config Request =====
      else if (prefix[0] == 'C' && prefix[1] == 'F') {
        clear_data_ready(); // Clear handshake GPIO
        ESP_LOGI(TAG, "Config request received");
        handle_config_request();
      }
      // ===== Branch: Polling packet (CF + zeros or all zeros) - ignore =====
      else if ((prefix[0] == 'C' && prefix[1] == 'F' && 
                packet.payload_length >= 4 && 
                packet.payload[2] == 0x00 && packet.payload[3] == 0x00) ||
               (prefix[0] == 0x00 && prefix[1] == 0x00)) {
        // This is a polling request from Master - already served via TX buffer
        ESP_LOGD(TAG, "Polling packet received (ignored)");
      }
      // ===== Branch: Unknown packet =====
      else {
        ESP_LOGW(TAG, "Unknown packet: [0]=0x%02X [1]=0x%02X (len=%u)",
                 prefix[0], prefix[1], packet.payload_length);
      }

    // ===== Branch: From Server Handler Task - Downlink to LAN =====
    if (xQueueReceive(g_downlink_queue, &downlink_item, 0) == pdTRUE) {
      clear_data_ready(); // Clear handshake GPIO
      ESP_LOGI(TAG, "Queueing downlink for handler %d (%u bytes)",
               downlink_item.target_id, downlink_item.length);
      // Cache downlink, do not load SPI TX here
      g_pending_downlink = downlink_item;
      g_pending_downlink_valid = true;
      signal_data_ready(); // WAN only notifies data-ready here
      // Queue receive immediately to be ready for DQ from Master
      lan_comm_queue_receive(g_lan_handle);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
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

  // Load into TX buffer and queue transaction immediately
  lan_comm_load_tx_data(g_lan_handle, (uint8_t *)&response, sizeof(response));
  lan_comm_queue_receive(g_lan_handle);  // Queue next transaction with response
  ESP_LOGD(TAG, "RTC response loaded: %s, Net: %d", response.rtc_string,
           response.network_status);
}

// ===== Send ACK to LAN =====
static void send_ack_to_lan(ack_type_t ack_type, uint8_t internet_flag) {
  uint8_t ack[3] = {FRAME_TYPE_ACK, ack_type, internet_flag};
  lan_comm_load_tx_data(g_lan_handle, ack, sizeof(ack));
  lan_comm_queue_receive(g_lan_handle);  // Queue next transaction with ACK
  vTaskDelay(pdMS_TO_TICKS(50));  // Prevent ACK loop - allow time between ACKs
  ESP_LOGI(TAG, "ACK sent: type=0x%02X, inet=0x%02X", ack_type, internet_flag);
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
  
  // Debug: Log original data from LAN
  ESP_LOGI(TAG, "Original data from LAN:");
  ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_length > 64 ? 64 : data_length, ESP_LOG_INFO);

  // Send ACK with internet status
  if (g_internet_status == INTERNET_STATUS_ONLINE) {
    // Build uplink buffer: [handler_type(3)] + [RTC+payload from LAN]
    uint16_t uplink_len = data_length + 3;
    uint8_t *uplink_buffer = (uint8_t *)malloc(uplink_len);
    if (uplink_buffer != NULL) {
      memcpy(uplink_buffer, handler_type, 3);           // Handler type first (CAN/LOR/ZIG/RS4)
      memcpy(&uplink_buffer[3], data, data_length);     // Then RTC + payload
      
      // Debug: Log uplink buffer before sending
      ESP_LOGI(TAG, "Uplink buffer (%u bytes):", uplink_len);
      ESP_LOG_BUFFER_HEXDUMP(TAG, uplink_buffer, uplink_len > 64 ? 64 : uplink_len, ESP_LOG_INFO);
      
      server_handler_enqueue_uplink(uplink_buffer, uplink_len);
      free(uplink_buffer);
    } else {
      ESP_LOGE(TAG, "Failed to allocate uplink buffer");
    }
    send_ack_to_lan(ACK_TYPE_RECEIVED_OK, ACK_TYPE_INTERNET_OK);
  } else {
    send_ack_to_lan(ACK_TYPE_RECEIVED_OK, ACK_TYPE_NO_INTERNET);
    // LAN MCU will save to SD card
  }
}

// ===== Handle Config Request =====
static void handle_config_request(void) {
  if (g_config_cache.has_config) {
    // Build config_data_t: [CF][length(2)][config_data]
    uint8_t config_packet[260];
    config_packet[0] = 'C';
    config_packet[1] = 'F';
    config_packet[2] = (g_config_cache.config_length >> 8) & 0xFF;
    config_packet[3] = g_config_cache.config_length & 0xFF;
    memcpy(&config_packet[4], g_config_cache.config_data,
           g_config_cache.config_length);

    // Load TX and queue transaction immediately
    lan_comm_load_tx_data(g_lan_handle, config_packet,
                          4 + g_config_cache.config_length);
    lan_comm_queue_receive(g_lan_handle);  // Queue next transaction with config
    // Mark config consumed; handshake state will be cleared in DQ handler
    g_config_cache.has_config = false;
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

  // Load data and queue transaction immediately
  lan_comm_load_tx_data(g_lan_handle, packet, packet_size);
  lan_comm_queue_receive(g_lan_handle);  // Queue next transaction with downlink
  ESP_LOGI(TAG, "Downlink data loaded for handler %s (%u bytes)",
           type_str, packet_size);

  // Wait for ACK from LAN MCU
  for (int retry = 0; retry < MAX_RETRY_COUNT; retry++) {
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

    ESP_LOGD(TAG, "Downlink ACK timeout, retry %d/%d", retry + 1,
             MAX_RETRY_COUNT);
  }

  clear_data_ready(); // Clear even on failure
  ESP_LOGD(TAG, "Downlink send failed after max retries");
}

// ===== Get RTC String =====
static void get_rtc_string(char *buffer) {
  struct tm timeinfo;
  memset(&timeinfo, 0, sizeof(struct tm));
  
  // Always use system time (already synced with RTC at boot, SNTP when online)
  time_t now = time(NULL);
  localtime_r(&now, &timeinfo);

  int year = timeinfo.tm_year + 1900;
  if (year > 9999) {
    year = 9999;
  } else if (year < 2000) {
    year = 2000; // Default to 2000 if invalid
  }

  // Format: "dd/mm/yyyy-hh:mm:ss"
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d-%02d:%02d:%02d", 
           timeinfo.tm_mday, timeinfo.tm_mon + 1, year, 
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  memcpy(buffer, tmp, 19);
  buffer[19] = '\0';

  // Update RTC cache
  if (g_rtc_cache.mutex && xSemaphoreTake(g_rtc_cache.mutex, 0) == pdTRUE) {
    memcpy(g_rtc_cache.rtc_string, buffer, 20);
    g_rtc_cache.valid = true;
    xSemaphoreGive(g_rtc_cache.mutex);
  }
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
  case HANDLER_RS485:
    return "RS4";
  default:
    return "UNK";
  }
}