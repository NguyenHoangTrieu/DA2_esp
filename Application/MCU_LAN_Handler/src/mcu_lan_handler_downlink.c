/**
 * @file mcu_lan_handler_downlink.c
 * @brief MCU LAN Handler - Downlink Manager (QSPI Slave, No Task)
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lan_comm.h"
#include "mcu_lan_handler.h"
#include "pcf8563_rtc.h"
#include "rom/ets_sys.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "MCU_LAN_DL";

// ===== Configuration =====
#define DOWNLINK_QUEUE_SIZE 20
#define MAX_DOWNLINK_PAYLOAD_SIZE 1024
#define GPIO_DATA_READY_PIN 8
#define GPIO_PULSE_WIDTH_US 10

// ===== Config Request State Machine (shared with uplink) =====
typedef enum {
  CONFIG_REQ_STATE_IDLE = 0,
  CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ,
  CONFIG_REQ_STATE_WAIT_CONFIG_RESP
} config_req_state_t;

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

// ===== Config Request Async Structure =====
typedef struct {
  config_request_t request;
  SemaphoreHandle_t completion_sem;
  esp_err_t result;
} config_request_async_t;

// ===== Global State (Shared with uplink) =====
lan_comm_handle_t g_lan_handle = NULL;
bool g_handler_running = false;
internet_status_t g_internet_status = INTERNET_STATUS_OFFLINE;

// ===== Downlink-Specific State =====
static QueueHandle_t g_downlink_queue = NULL;
SemaphoreHandle_t g_config_mutex = NULL;
static config_cache_t g_config_cache = {0};
bool g_config_cache_has_config = false; // Exposed to uplink

// Pending downlink (accessed by uplink task)
extern downlink_item_t g_pending_downlink;
extern bool g_pending_downlink_valid;

// Config request state (accessed by uplink)
config_req_state_t g_config_req_state = CONFIG_REQ_STATE_IDLE;
config_request_t *g_active_config_request_ptr = NULL;
bool g_active_config_request_valid = false;
static config_request_async_t *g_config_req_async = NULL;

// ===== Forward Declarations =====
static esp_err_t setup_data_ready_gpio(void);
static void signal_data_ready(void);
static void clear_data_ready(void);

// External from uplink module
extern esp_err_t mcu_lan_handler_start_uplink_task(void);
extern void mcu_lan_handler_stop_uplink_task(void);

// ===== GPIO Data-Ready Implementation =====
static esp_err_t setup_data_ready_gpio(void) {
  gpio_config_t io_conf = {.pin_bit_mask = BIT64(GPIO_DATA_READY_PIN),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure GPIO%d: %s", GPIO_DATA_READY_PIN,
             esp_err_to_name(ret));
    return ret;
  }

  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  ESP_LOGI(TAG, "GPIO%d configured for data-ready signaling (LOW idle)",
           GPIO_DATA_READY_PIN);
  return ESP_OK;
}

static void signal_data_ready(void) {
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  ets_delay_us(GPIO_PULSE_WIDTH_US);
  gpio_set_level(GPIO_DATA_READY_PIN, 1);
  ESP_LOGD(TAG, "Data-ready pulse sent (GPIO%d, %uμs)", GPIO_DATA_READY_PIN,
           GPIO_PULSE_WIDTH_US);
}

static void clear_data_ready(void) {
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  ESP_LOGV(TAG, "Data-ready cleared (GPIO%d LOW)", GPIO_DATA_READY_PIN);
}

// ===== RTC Response Helper =====
void downlink_send_rtc_response(void) {
  struct tm timeinfo;
  memset(&timeinfo, 0, sizeof(struct tm));
  time_t now = time(NULL);
  localtime_r(&now, &timeinfo);

  int year = timeinfo.tm_year + 1900;
  if (year > 9999)
    year = 9999;
  else if (year < 2000)
    year = 2000;

  rtc_config_response_t response;
  response.prefix[0] = 'R';
  response.prefix[1] = 'T';

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(response.rtc_string, sizeof(response.rtc_string),
           "%02d/%02d/%04d-%02d:%02d:%02d", (int)timeinfo.tm_mday,
           (int)(timeinfo.tm_mon + 1), year, (int)timeinfo.tm_hour,
           (int)timeinfo.tm_min, (int)timeinfo.tm_sec);
#pragma GCC diagnostic pop

  response.rtc_string[sizeof(response.rtc_string) - 1] = '\0';
  response.network_status =
      (g_internet_status == INTERNET_STATUS_ONLINE) ? 1 : 0;

  lan_comm_load_tx_data(g_lan_handle, (uint8_t *)&response, sizeof(response));
  ESP_LOGI(TAG, "RTC response loaded: %s, net=%s", response.rtc_string,
           response.network_status ? "ONLINE" : "OFFLINE");
}

// ===== ACK Response Helper =====
void downlink_send_ack_to_lan(ack_type_t ack_type, uint8_t internet_flag) {
  uint8_t ack[3];
  ack[0] = FRAME_TYPE_ACK;
  ack[1] = ack_type;
  ack[2] = internet_flag;
  lan_comm_load_tx_data(g_lan_handle, ack, sizeof(ack));
  ESP_LOGD(TAG, "ACK sent: type=0x%02X, internet=%u", ack_type, internet_flag);
}

// ===== Config Request Handler =====
void downlink_handle_config_request(void) {
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW(TAG, "Config mutex timeout");
    return;
  }

  if (!g_config_cache.has_config) {
    xSemaphoreGive(g_config_mutex);
    ESP_LOGW(TAG, "Config request but cache empty");
    return;
  }

  // Build config packet: [CF][length(2)][config_data]
  uint16_t packet_size = 4 + g_config_cache.config_length;
  uint8_t *config_packet = (uint8_t *)malloc(packet_size);
  if (config_packet == NULL) {
    xSemaphoreGive(g_config_mutex);
    ESP_LOGE(TAG, "Failed to allocate config packet");
    return;
  }

  config_packet[0] = (WAN_COMM_HEADER_CF >> 8) & 0xFF;
  config_packet[1] = WAN_COMM_HEADER_CF & 0xFF;
  config_packet[2] = (g_config_cache.config_length >> 8) & 0xFF;
  config_packet[3] = g_config_cache.config_length & 0xFF;
  memcpy(&config_packet[4], g_config_cache.config_data,
         g_config_cache.config_length);

  lan_comm_load_tx_data(g_lan_handle, config_packet, packet_size);
  ESP_LOGI(TAG, "Config %s loaded (%u bytes)",
           g_config_cache.is_fota ? "FOTA" : "DATA", packet_size);

  g_config_cache.has_config = false;
  g_config_cache_has_config = false;
  free(config_packet);
  xSemaphoreGive(g_config_mutex);
}

// ===== Public API: Start Handler =====
esp_err_t mcu_lan_handler_start(void) {
  if (g_handler_running) {
    ESP_LOGW(TAG, "Handler already running");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "╔════════════════════════════════════════════════════╗");
  ESP_LOGI(TAG, "║   MCU LAN Handler (QSPI Slave - WAN MCU)          ║");
  ESP_LOGI(TAG, "╚════════════════════════════════════════════════════╝");

  if (setup_data_ready_gpio() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to setup GPIO%d", GPIO_DATA_READY_PIN);
    return ESP_FAIL;
  }

  if (pcf8563_init() != ESP_OK) {
    ESP_LOGW(TAG, "PCF8563 init failed, using system time fallback");
  } else {
    struct tm rtc_timeinfo;
    if (pcf8563_read_time(&rtc_timeinfo) == ESP_OK) {
      time_t rtc_time = mktime(&rtc_timeinfo);
      struct timeval tv = {.tv_sec = rtc_time, .tv_usec = 0};
      settimeofday(&tv, NULL);
      ESP_LOGI(TAG,
               "System time synced from PCF8563: %02d/%02d/%04d-%02d:%02d:%02d",
               rtc_timeinfo.tm_mday, rtc_timeinfo.tm_mon + 1,
               rtc_timeinfo.tm_year + 1900, rtc_timeinfo.tm_hour,
               rtc_timeinfo.tm_min, rtc_timeinfo.tm_sec);
    }
  }

  // Initialize LAN communication (QSPI Slave HD)
  lan_comm_config_t lan_config = {.gpio_sck = 12,
                                  .gpio_cs = 10,
                                  .gpio_io0 = 11,
                                  .gpio_io1 = 13,
                                  .gpio_io2 = 14,
                                  .gpio_io3 = 15,
                                  .gpio_data_ready = GPIO_DATA_READY_PIN,
                                  .mode = 0,
                                  .host_id = SPI2_HOST,
                                  .dma_channel = SPI_DMA_CH_AUTO,
                                  .rx_buffer_size = 8192,
                                  .tx_buffer_size = 8192,
                                  .enable_quad_mode = true,
                                  .auto_signal_data_ready = false};

  lan_comm_status_t status = lan_comm_init(&lan_config, &g_lan_handle);
  if (status != LAN_COMM_OK) {
    ESP_LOGE(TAG, "Failed to initialize LAN comm (QSPI Slave): %d", status);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "QSPI Slave initialized: 40 MHz, 8KB buffers, QIO mode");

  g_downlink_queue = xQueueCreate(DOWNLINK_QUEUE_SIZE, sizeof(downlink_item_t));
  if (g_downlink_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create downlink queue");
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  g_config_mutex = xSemaphoreCreateMutex();
  if (g_config_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create config mutex");
    vQueueDelete(g_downlink_queue);
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  g_handler_running = true;

  if (mcu_lan_handler_start_uplink_task() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start uplink task");
    g_handler_running = false;
    vSemaphoreDelete(g_config_mutex);
    vQueueDelete(g_downlink_queue);
    lan_comm_deinit(g_lan_handle);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MCU LAN Handler started successfully");
  ESP_LOGI(TAG, "  Downlink queue: %d items × %u bytes", DOWNLINK_QUEUE_SIZE,
           MAX_DOWNLINK_PAYLOAD_SIZE);
  ESP_LOGI(TAG, "  GPIO data-ready: GPIO%d (pulse width %uμs)",
           GPIO_DATA_READY_PIN, GPIO_PULSE_WIDTH_US);

  return ESP_OK;
}

// ===== Public API: Stop Handler =====
esp_err_t mcu_lan_handler_stop(void) {
  if (!g_handler_running) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Stopping MCU LAN Handler");
  g_handler_running = false;

  mcu_lan_handler_stop_uplink_task();
  pcf8563_deinit();

  if (g_config_mutex) {
    vSemaphoreDelete(g_config_mutex);
    g_config_mutex = NULL;
  }
  if (g_downlink_queue) {
    vQueueDelete(g_downlink_queue);
    g_downlink_queue = NULL;
  }
  if (g_lan_handle) {
    lan_comm_deinit(g_lan_handle);
    g_lan_handle = NULL;
  }

  gpio_reset_pin(GPIO_DATA_READY_PIN);
  ESP_LOGI(TAG, "MCU LAN Handler stopped");
  return ESP_OK;
}

// ===== Public API: Set Internet Status =====
void mcu_lan_handler_set_internet_status(internet_status_t status) {
  if (g_internet_status != status) {
    g_internet_status = status;
    ESP_LOGI(TAG, "Internet status: %s",
             status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");
  }
}

// ===== Public API: Enqueue Downlink =====
bool mcu_lan_enqueue_downlink(handler_id_t target_id, uint8_t *data,
                              uint16_t len) {
  if (g_downlink_queue == NULL || data == NULL || len == 0) {
    ESP_LOGE(TAG, "Invalid downlink parameters");
    return false;
  }

  if (len > MAX_DOWNLINK_PAYLOAD_SIZE) {
    ESP_LOGE(TAG, "Downlink data too large: %u > %u", len,
             MAX_DOWNLINK_PAYLOAD_SIZE);
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

  ESP_LOGI(TAG, "Downlink queued: handler=%s, len=%u",
           handler_id_to_string(target_id), len);

  // Cache downlink for uplink task
  g_pending_downlink = item;
  g_pending_downlink_valid = true;

  // Signal data-ready to LAN MCU
  signal_data_ready();

  ESP_LOGI(TAG, "Downlink cached, GPIO%d signaled", GPIO_DATA_READY_PIN);
  return true;
}

// ===== Public API: Update Config Cache =====
void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length,
                                   bool is_fota) {
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Config mutex timeout");
    return;
  }

  if (config_data && length > 0 &&
      length <= sizeof(g_config_cache.config_data)) {
    memcpy(g_config_cache.config_data, config_data, length);
    g_config_cache.config_length = length;
    g_config_cache.has_config = true;
    g_config_cache.is_fota = is_fota;
    g_config_cache_has_config = true;

    ESP_LOGI(TAG, "Config cached: %u bytes, FOTA=%s", length,
             is_fota ? "YES" : "NO");
    signal_data_ready();
  }

  xSemaphoreGive(g_config_mutex);
}

// ===== Public API: Request LAN Config Async =====
esp_err_t mcu_lan_handler_request_config_async(uint8_t *buffer,
                                               uint16_t *out_len,
                                               uint16_t max_len,
                                               uint32_t timeout_ms) {
  if (buffer == NULL || out_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Requesting config from LAN MCU (async)");

  if (g_active_config_request_valid) {
    ESP_LOGW(TAG, "Config request already in progress");
    return ESP_ERR_INVALID_STATE;
  }

  g_config_req_async =
      (config_request_async_t *)malloc(sizeof(config_request_async_t));
  if (g_config_req_async == NULL) {
    ESP_LOGE(TAG, "Failed to allocate config request");
    return ESP_ERR_NO_MEM;
  }

  g_config_req_async->completion_sem = xSemaphoreCreateBinary();
  if (g_config_req_async->completion_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create completion semaphore");
    free(g_config_req_async);
    g_config_req_async = NULL;
    return ESP_ERR_NO_MEM;
  }

  g_config_req_async->request.type = CONFIG_REQ_LAN_CONFIG;
  g_config_req_async->request.response_buffer = buffer;
  g_config_req_async->request.response_len = out_len;
  g_config_req_async->request.buffer_size = max_len;
  g_config_req_async->request.completion_sem =
      g_config_req_async->completion_sem;
  g_config_req_async->request.result = ESP_FAIL;
  g_config_req_async->result = ESP_FAIL;

  g_active_config_request_ptr = &g_config_req_async->request;
  g_active_config_request_valid = true;
  g_config_req_state = CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ;

  ESP_LOGI(TAG, "Config request registered (heap @ %p)", g_config_req_async);
  signal_data_ready();

  esp_err_t result;
  if (xSemaphoreTake(g_config_req_async->completion_sem,
                     pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGE(TAG, "Config request timeout (%lu ms)", timeout_ms);
    clear_data_ready();
    result = ESP_ERR_TIMEOUT;
    goto cleanup;
  }

  result = g_config_req_async->request.result;
  ESP_LOGI(TAG, "Config request completed: %s", esp_err_to_name(result));

cleanup:
  if (g_config_req_async) {
    if (g_config_req_async->completion_sem) {
      vSemaphoreDelete(g_config_req_async->completion_sem);
    }
    free(g_config_req_async);
    g_config_req_async = NULL;
  }

  g_active_config_request_valid = false;
  g_active_config_request_ptr = NULL;
  g_config_req_state = CONFIG_REQ_STATE_IDLE;

  return result;
}

// ===== Public API: Get Internet Status =====
internet_status_t mcu_lan_handler_get_internet_status(void) {
  return g_internet_status;
}
