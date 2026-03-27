/**
 * @file mcu_lan_handler_uplink.c
 * @brief MCU LAN Handler - Uplink Processor Task (SPI Slave, Priority 6)
 */
#include "DA2_esp.h"
#include "config_handler.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"  // For USB source response routing
#include "esp_log.h"
#include "esp_timer.h"
#include "fota_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lan_comm.h"
#include "mcu_lan_handler.h"
#include "mqtt_handler.h"
#include "http_handler.h"
#include "coap_handler.h"
#include "pcf8563_rtc.h"
#include "rbg_handler.h"
#include "ppp_server.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "MCU_LAN_UL";

// ===== Configuration =====
#define RX_TIMEOUT_MS 100
#define ACK_WAIT_TIMEOUT_MS 500
#define MAX_RETRY_COUNT 3
#define GPIO_DATA_READY_PIN 8
#define LAN_FOTA_TIMEOUT_MS 300000  // 300 seconds timeout for LAN FOTA completion

// ===== Config Request State Machine =====
typedef enum {
  CONFIG_REQ_STATE_IDLE = 0,
  CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ,
  CONFIG_REQ_STATE_WAIT_CONFIG_RESP
} config_req_state_t;

// ===== External from downlink module =====
extern lan_comm_handle_t g_lan_handle;
extern bool g_handler_running;
extern internet_status_t g_internet_status;
extern SemaphoreHandle_t g_config_mutex;
extern config_req_state_t g_config_req_state;
extern config_request_t *g_active_config_request_ptr;
extern bool g_active_config_request_valid;
extern bool g_config_cache_has_config;
extern bool g_fota_request_pending;

// Forward declarations from downlink module
extern void downlink_send_rtc_response(void);
extern void downlink_handle_config_request(void);
extern void downlink_send_ack_to_lan(ack_type_t ack_type,
                                     uint8_t internet_flag);

// ===== Uplink Module State =====
static uint32_t g_cached_lan_fw_version = 0;
static uint32_t g_cached_wan_fw_version = 0;
static bool g_waiting_for_lan_update = false;     // LAN is updating via FOTA
static bool g_wan_fota_in_progress = false;       // WAN FOTA is running (prevents re-trigger)
static TaskHandle_t g_uplink_task_handle = NULL;
static TaskHandle_t g_fota_task_handle = NULL;
static SemaphoreHandle_t g_fota_start_sem = NULL;
static bool g_fota_pending = false;
static bool g_fota_trigger_pending = false;  // Flag: FOTA trigger ready to send to LAN
static bool g_fota_pending_internet = false;  // FOTA needed but waiting for internet
static uint32_t g_pending_lan_version_for_fota = 0;  // Cached version while waiting
static internet_status_t g_prev_internet_status = INTERNET_STATUS_OFFLINE;  // Track status changes
static TickType_t g_lan_fota_wait_start_tick = 0;  // Track when LAN FOTA trigger was sent

// Pending downlink (set by downlink module)
typedef struct {
  handler_id_t target_id;
  uint8_t data[1024];
  uint16_t length;
} downlink_item_t;

downlink_item_t g_pending_downlink;
bool g_pending_downlink_valid = false;

// External reference
extern config_server_type_t g_server_type;

// ===== RTC Cache (Thread-safe) =====
typedef struct {
  char rtc_string[20];
  bool valid;
  SemaphoreHandle_t mutex;
} rtc_cache_t;

static rtc_cache_t g_rtc_cache = {.valid = false};

// ===== Forward Declarations =====
static void uplink_processor_task(void *pvParameters);
static void fota_task(void *pvParameters);
static esp_err_t perform_handshake_slave(void);
static void process_handshake(const uint8_t *payload, uint16_t length);
static void process_data_from_lan(const uint8_t *payload, uint16_t length);
static void process_data_query(void);
static void process_config_query(const lan_comm_packet_t *packet);
static bool check_fota_required(uint32_t received_lan_version);
static void send_downlink_to_lan(const downlink_item_t *item);
static void clear_data_ready(void);
static void send_fota_trigger_to_lan(void);

// ===== Helper: Clear Data Ready GPIO =====
static void clear_data_ready(void) {
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  ESP_LOGV(TAG, "Data-ready cleared (GPIO%d LOW)", GPIO_DATA_READY_PIN);
}

// ===== Uplink Task Entry Point =====
esp_err_t mcu_lan_handler_start_uplink_task(void) {
  // Create RTC cache mutex
  g_rtc_cache.mutex = xSemaphoreCreateMutex();
  if (g_rtc_cache.mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create RTC mutex");
    return ESP_FAIL;
  }

  // Create uplink processor task (Priority 6)
  BaseType_t ret = xTaskCreate(uplink_processor_task, "lan_uplink", 6144, NULL,
                               6, &g_uplink_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create uplink task");
    vSemaphoreDelete(g_rtc_cache.mutex);
    return ESP_FAIL;
  }

  g_fota_start_sem = xSemaphoreCreateBinary();
  if (g_fota_start_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create FOTA semaphore");
    vTaskDelete(g_uplink_task_handle);
    g_uplink_task_handle = NULL;
    vSemaphoreDelete(g_rtc_cache.mutex);
    return ESP_FAIL;
  }

  ret = xTaskCreate(fota_task, "lan_fota", 4096, NULL, 5, &g_fota_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create FOTA task");
    vSemaphoreDelete(g_fota_start_sem);
    g_fota_start_sem = NULL;
    vTaskDelete(g_uplink_task_handle);
    g_uplink_task_handle = NULL;
    vSemaphoreDelete(g_rtc_cache.mutex);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Uplink processor task started (Priority 6, stack 6KB)");
  return ESP_OK;
}

void mcu_lan_handler_stop_uplink_task(void) {
  if (g_uplink_task_handle) {
    vTaskDelete(g_uplink_task_handle);
    g_uplink_task_handle = NULL;
  }
  if (g_fota_task_handle) {
    vTaskDelete(g_fota_task_handle);
    g_fota_task_handle = NULL;
  }
  if (g_fota_start_sem) {
    vSemaphoreDelete(g_fota_start_sem);
    g_fota_start_sem = NULL;
  }
  if (g_rtc_cache.mutex) {
    vSemaphoreDelete(g_rtc_cache.mutex);
    g_rtc_cache.mutex = NULL;
  }
}

// ===== Main Uplink Task =====
static void uplink_processor_task(void *pvParameters) {
  ESP_LOGI(TAG, " Uplink Processor (SPI Slave, Priority 6) started ");

  // ===== Phase 1: Handshake =====
  ESP_LOGI(TAG, "Phase 1: Waiting for handshake from LAN MCU");
  while (g_handler_running) {
    if (perform_handshake_slave() == ESP_OK) {
      ESP_LOGI(TAG, "Handshake complete, entering SPI receive loop");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // Check if server FOTA was triggered during handshake
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (g_fota_request_pending) {
      ESP_LOGW(TAG, "[FOTA] Server command detected - waiting for LAN update");
      g_waiting_for_lan_update = true;
    }
    xSemaphoreGive(g_config_mutex);
  }

  // ===== Phase 2: SPI Receive Loop =====
  ESP_LOGI(TAG, "Phase 2: SPI Receive Loop");
  
  while (g_handler_running) {
    
    // ===== Timeout Check: LAN FOTA waiting timeout =====
    if (g_waiting_for_lan_update && g_lan_fota_wait_start_tick > 0) {
      TickType_t elapsed = xTaskGetTickCount() - g_lan_fota_wait_start_tick;
      if (elapsed >= pdMS_TO_TICKS(LAN_FOTA_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "[TIMEOUT] LAN FOTA handshake not received after %d seconds", LAN_FOTA_TIMEOUT_MS/1000);
        ESP_LOGE(TAG, "[TIMEOUT] LAN may have failed FOTA or stuck - resetting wait flag");
        g_waiting_for_lan_update = false;
        g_lan_fota_wait_start_tick = 0;
      }
    }
    
    // ===== Periodic Check: FOTA pending internet trigger =====
    // Check if previously pending FOTA can now be triggered (internet came online)
    if (g_fota_pending_internet && g_internet_status == INTERNET_STATUS_ONLINE && 
        g_prev_internet_status == INTERNET_STATUS_OFFLINE) {
      ESP_LOGI(TAG, "[FOTA] Internet just came online, triggering pending FOTA");
      g_not_ppp_to_lan = true;
      if (!ppp_server_is_initialized()) {
        ppp_server_init();
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      send_fota_trigger_to_lan();
      g_waiting_for_lan_update = true;
      g_lan_fota_wait_start_tick = xTaskGetTickCount();  // Start timeout timer
      g_fota_pending_internet = false;
    }
    g_prev_internet_status = g_internet_status;
    
    // Pre-queue RX transaction (mandatory for SPI slave)
    lan_comm_queue_receive(g_lan_handle);

    // Block on completion (100ms timeout)
    lan_comm_packet_t packet;
    lan_comm_status_t status =
        lan_comm_get_received_packet(g_lan_handle, &packet, RX_TIMEOUT_MS);

    // Skip invalid packets (polling garbage, timeout, etc.)
    if (status != LAN_COMM_OK || packet.payload_length < 2) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Handle FOTA handshake ACK without blocking the RX loop.
    if (g_fota_pending && packet.payload_length >= 2 &&
        packet.payload[0] == FRAME_TYPE_ACK &&
        packet.payload[1] == ACK_TYPE_HANDSHAKE) {
      ESP_LOGI(TAG, "FOTA handshake ACK received from LAN MCU");
      g_fota_pending = false;
      g_fota_request_pending = false;
      if (g_fota_start_sem) {
        xSemaphoreGive(g_fota_start_sem);
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Extract header (big-endian)
    uint16_t header = (packet.payload[0] << 8) | packet.payload[1];
    ESP_LOGI(TAG, "SPI RX: header=0x%04X, len=%u", header,
             packet.payload_length);

    // ===== Dispatch Frame =====
    switch (header) {
    case WAN_COMM_HEADER_CF: // Command Frame
      if (packet.payload_length >= 4) {
        // Check subtype
        if (packet.payload[2] == 0x01) {
          // Handshake request
          process_handshake(packet.payload, packet.payload_length);
        } else if (packet.payload[2] == 'R' && packet.payload[3] == 'T') {
          // RTC request
          ESP_LOGI(TAG, "RTC request received");
          // Avoid overwriting TX buffer while config/downlink is pending.
          if (g_config_cache_has_config || g_active_config_request_valid ||
              g_pending_downlink_valid) {
            ESP_LOGI(TAG, "RTC response deferred (pending TX payload)");
          } else {
            downlink_send_rtc_response();
          }
        } else if (packet.payload[2] == 'C' && packet.payload[3] == 'F') {
          // Config request
          ESP_LOGI(TAG, "Config request received");
          clear_data_ready();
          downlink_handle_config_request();
        } else if (packet.payload[2] == 'C' && packet.payload[3] == 'Q') {
          // Config query from LAN
          ESP_LOGI(TAG, "Config query received from LAN");
          process_config_query(&packet);
        }
        // Note: FOTA trigger from LAN uses format [CF][FW] but WAN doesn't receive it
        // WAN only sends FOTA trigger to LAN, never receives it from LAN
      }
      break;

    case WAN_COMM_HEADER_DT: // Data Frame
      process_data_from_lan(packet.payload, packet.payload_length);
      break;

    case WAN_COMM_HEADER_DQ: // Data Query from LAN
      ESP_LOGI(TAG, "DQ request received from LAN MCU");
      process_data_query();
      break;

    default:
      ESP_LOGW(TAG, "Unknown header: 0x%04X", header);
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }

  ESP_LOGI(TAG, "Uplink processor task exiting");
  vTaskDelete(NULL);
}

// ===== FOTA Requirement Check =====
// Wait for internet and trigger FOTA when online (similar to config_handler MCU_LAN case)
static void trigger_lan_fota_if_needed(uint32_t received_lan_version) {
  g_cached_lan_fw_version = received_lan_version;
  g_cached_wan_fw_version = WAN_FW_VERSION;

  uint8_t lan_maj = FW_VERSION_MAJOR(received_lan_version);
  uint8_t lan_min = FW_VERSION_MINOR(received_lan_version);
  uint8_t lan_pat = FW_VERSION_PATCH(received_lan_version);
  uint8_t lan_bld = FW_VERSION_BUILD(received_lan_version);
  uint8_t wan_maj = FW_VERSION_MAJOR(WAN_FW_VERSION);
  uint8_t wan_min = FW_VERSION_MINOR(WAN_FW_VERSION);
  uint8_t wan_pat = FW_VERSION_PATCH(WAN_FW_VERSION);
  uint8_t wan_bld = FW_VERSION_BUILD(WAN_FW_VERSION);

  // Check for version mismatch or server FOTA command
  bool version_mismatch = (lan_maj != wan_maj || lan_min != wan_min ||
                           lan_pat != wan_pat || lan_bld != wan_bld);
  
  bool server_fota = false;
  if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    server_fota = g_fota_request_pending;
    xSemaphoreGive(g_config_mutex);
  }

  if (!version_mismatch && !server_fota) {
    return;  // No FOTA needed
  }

  // If already waiting for LAN update or WAN FOTA in progress, don't trigger again
  if (g_waiting_for_lan_update || g_wan_fota_in_progress) {
    return;
  }

  if (version_mismatch) {
    ESP_LOGW(TAG, "[MISMATCH] Detected: LAN v%u.%u.%u.%u vs WAN v%u.%u.%u.%u",
             lan_maj, lan_min, lan_pat, lan_bld, wan_maj, wan_min, wan_pat,
             wan_bld);
  }
  if (server_fota) {
    ESP_LOGW(TAG, "[FOTA] Server command detected");
  }

  // Check internet status
  if (g_internet_status != INTERNET_STATUS_ONLINE) {
    ESP_LOGW(TAG, "[FOTA] Internet offline, marking FOTA pending - will trigger when online");
    g_fota_pending_internet = true;
    g_pending_lan_version_for_fota = received_lan_version;
    return;
  }

  // Internet is online - proceed with FOTA trigger (following config_handler MCU_LAN pattern)
  ESP_LOGI(TAG, "[FOTA] Internet online, initializing PPP server for LAN");
  g_not_ppp_to_lan = true;
  if (!ppp_server_is_initialized()) {
    ppp_server_init();
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  // Trigger LAN FOTA via DQ
  send_fota_trigger_to_lan();
  g_waiting_for_lan_update = true;
  g_lan_fota_wait_start_tick = xTaskGetTickCount();  // Start timeout timer
  g_fota_pending_internet = false;  // Clear pending flag
  g_wan_fota_in_progress = false;  // Reset for next FOTA cycle
}

// ===== Handshake Implementation =====
static esp_err_t perform_handshake_slave(void) {
  // Pre-queue RX
  lan_comm_queue_receive(g_lan_handle);

  // Wait for handshake request
  lan_comm_packet_t packet;
  lan_comm_status_t status =
      lan_comm_get_received_packet(g_lan_handle, &packet, 500);
  if (status != LAN_COMM_OK) {
    ESP_LOGD(TAG, "Handshake RX timeout or error: %d", status);
    return ESP_FAIL;
  }

  // Debug: Log received packet
  ESP_LOGI(TAG, "RX packet: len=%u", packet.payload_length);
  ESP_LOG_BUFFER_HEXDUMP(TAG, packet.payload, 
                         packet.payload_length > 32 ? 32 : packet.payload_length, 
                         ESP_LOG_INFO);

  // Check for handshake request: [CF][0x01][fw_version(4)]
  if (packet.payload_length >= 7 && packet.payload[0] == 0x43 &&
      packet.payload[1] == 0x46 && packet.payload[2] == 0x01) {
    // Extract LAN firmware version (big-endian)
    uint32_t lan_fw_version = ((uint32_t)packet.payload[3] << 24) |
                              ((uint32_t)packet.payload[4] << 16) |
                              ((uint32_t)packet.payload[5] << 8) |
                              ((uint32_t)packet.payload[6]);

    ESP_LOGI(TAG, "Handshake request: LAN FW v%u.%u.%u.%u",
             FW_VERSION_MAJOR(lan_fw_version), FW_VERSION_MINOR(lan_fw_version),
             FW_VERSION_PATCH(lan_fw_version),
             FW_VERSION_BUILD(lan_fw_version));

    // Check if FOTA needed and trigger if conditions met
    trigger_lan_fota_if_needed(lan_fw_version);

    // Build handshake response: [ACK][0x10][internet_flag][wan_fw_version(4)]
    uint8_t response[7];
    response[0] = FRAME_TYPE_ACK;
    response[1] = ACK_TYPE_HANDSHAKE;
    response[2] = (g_internet_status == INTERNET_STATUS_ONLINE) ? 1 : 0;
    response[3] = (WAN_FW_VERSION >> 24) & 0xFF;
    response[4] = (WAN_FW_VERSION >> 16) & 0xFF;
    response[5] = (WAN_FW_VERSION >> 8) & 0xFF;
    response[6] = WAN_FW_VERSION & 0xFF;

    // Load TX buffer
    ESP_LOGI(TAG, "Sending handshake response:");
    ESP_LOG_BUFFER_HEXDUMP(TAG, response, sizeof(response), ESP_LOG_INFO);
    
    lan_comm_load_tx_data(g_lan_handle, response, sizeof(response));
    ESP_LOGI(TAG,
             "Handshake complete: Internet=%s, WAN FW=v%u.%u.%u.%u",
             response[2] ? "ONLINE" : "OFFLINE", WAN_FW_VERSION_MAJOR,
             WAN_FW_VERSION_MINOR, WAN_FW_VERSION_PATCH, WAN_FW_VERSION_BUILD);

    return ESP_OK;
  }

  return ESP_FAIL;
}

// ===== Process Handshake (mid-operation) =====
static void process_handshake(const uint8_t *payload, uint16_t length) {
  if (length < 7) {
    ESP_LOGE(TAG, "Handshake packet too short");
    return;
  }

  // Extract LAN firmware version
  uint32_t lan_fw_version =
      ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) |
      ((uint32_t)payload[5] << 8) | ((uint32_t)payload[6]);

  ESP_LOGI(TAG, "Mid-operation handshake: LAN FW v%u.%u.%u.%u",
           FW_VERSION_MAJOR(lan_fw_version), FW_VERSION_MINOR(lan_fw_version),
           FW_VERSION_PATCH(lan_fw_version), FW_VERSION_BUILD(lan_fw_version));

  // ===== CASE 1: LAN finished FOTA, now WAN starts its own FOTA =====
  if (g_waiting_for_lan_update) {
    ESP_LOGW(TAG, "[MISMATCH] LAN FOTA complete (v%u.%u.%u.%u), WAN FOTA starting (NO ACK)",
             FW_VERSION_MAJOR(lan_fw_version), FW_VERSION_MINOR(lan_fw_version),
             FW_VERSION_PATCH(lan_fw_version), FW_VERSION_BUILD(lan_fw_version));
    g_cached_lan_fw_version = lan_fw_version;
    g_waiting_for_lan_update = false;
    g_lan_fota_wait_start_tick = 0;  // Clear timeout timer
    g_wan_fota_in_progress = true;  // Prevent re-triggering while WAN FOTA runs
    // DO NOT send ACK - let LAN retry handshake
    // Start WAN FOTA immediately
    fota_handler_task_start();
    return;  // Exit without ACK
  }

  // ===== CASE 2: Normal mid-operation handshake =====
  // If WAN FOTA is in progress, don't send ACK - let LAN keep retrying
  if (g_wan_fota_in_progress) {
    ESP_LOGD(TAG, "WAN FOTA in progress, skipping handshake ACK");
    return;
  }
  
  // Check if FOTA needed - internet check happens in main loop now
  trigger_lan_fota_if_needed(lan_fw_version);

  // Send handshake response
  uint8_t response[7];
  response[0] = FRAME_TYPE_ACK;
  response[1] = ACK_TYPE_HANDSHAKE;
  response[2] = (g_internet_status == INTERNET_STATUS_ONLINE) ? 1 : 0;
  response[3] = (WAN_FW_VERSION >> 24) & 0xFF;
  response[4] = (WAN_FW_VERSION >> 16) & 0xFF;
  response[5] = (WAN_FW_VERSION >> 8) & 0xFF;
  response[6] = WAN_FW_VERSION & 0xFF;

  lan_comm_load_tx_data(g_lan_handle, response, sizeof(response));
  ESP_LOGI(TAG, "Handshake ACK sent in data mode");
}

// ===== Process Data from LAN =====
static void process_data_from_lan(const uint8_t *payload, uint16_t length) {
  // Format: [DT][handler_type(3)][data_length(2)][data_payload]
  if (length < DATA_PACKET_HEADER_SIZE) {
    ESP_LOGE(TAG, "Data packet too short: %u bytes", length);
    return;
  }

  // Extract handler type and length
  uint8_t handler_type[4] = {payload[2], payload[3], payload[4], '\0'};
  uint16_t data_length = (payload[5] << 8) | payload[6];

  ESP_LOGI(TAG, "Data from LAN: handler=%s, len=%u", handler_type, data_length);

  // Validate length
  if (length < DATA_PACKET_HEADER_SIZE + data_length) {
    ESP_LOGE(TAG, "Data payload incomplete");
    return;
  }

  // Debug: Log data from LAN
  ESP_LOGI(TAG, "Original data from LAN:");
  ESP_LOG_BUFFER_HEXDUMP(TAG, &payload[DATA_PACKET_HEADER_SIZE],
                         data_length > 64 ? 64 : data_length, ESP_LOG_INFO);

  // Route response based on the original command source
  command_source_t src = mcu_lan_handler_get_config_source();
  ESP_LOGI(TAG, "Response source: %s",
           (src == CMD_SOURCE_UART) ? "UART" :
           (src == CMD_SOURCE_USB)  ? "USB"  :
           (src == CMD_SOURCE_HTTP) ? "HTTP/WebApp" :
           (src == CMD_SOURCE_HTTP_RPC) ? "HTTP/RPC" :
           (src == CMD_SOURCE_COAP) ? "CoAP" : "SERVER/MQTT");

  if (src == CMD_SOURCE_UART || src == CMD_SOURCE_USB) {
    // Local app command → extract and send back to the originating interface
    const uint8_t *response_data = &payload[DATA_PACKET_HEADER_SIZE];
    uint16_t response_len = data_length;

    // Search for "CFBL:" or "BR:" marker as start of actual response
    const uint8_t *marker = (const uint8_t *)strstr((const char *)response_data, "CFBL:");
    if (marker == NULL) {
      marker = (const uint8_t *)strstr((const char *)response_data, "BR:");
    }
    if (marker != NULL) {
      size_t offset = marker - response_data;
      response_data = marker;
      response_len  = data_length - (uint16_t)offset;
      ESP_LOGI(TAG, "Found response marker at offset %zu, len=%u", offset, response_len);
    }

    if (src == CMD_SOURCE_UART) {
      int sent = uart_write_bytes(UART_NUM_0, (const char *)response_data, response_len);
      if (sent > 0) {
        uart_write_bytes(UART_NUM_0, "\n", 1);  // Line terminator for App
        ESP_LOGI(TAG, "Response sent to UART: %d bytes", sent);
      } else {
        ESP_LOGE(TAG, "Failed to send response to UART");
      }
    } else {
      // CMD_SOURCE_USB
      int sent = usb_serial_jtag_write_bytes((const char *)response_data, response_len,
                                             pdMS_TO_TICKS(100));
      if (sent > 0) {
        usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(50));  // Line terminator for App
        ESP_LOGI(TAG, "Response sent to USB: %d bytes", sent);
      } else {
        ESP_LOGW(TAG, "USB response failed (may not be connected)");
      }
    }

    // ACK to LAN MCU
    downlink_send_ack_to_lan(ACK_TYPE_RECEIVED_OK,
                             g_internet_status == INTERNET_STATUS_ONLINE ? 1 : 0);
    ESP_LOGI(TAG, "Local response forwarded, ACK sent to LAN");
    return;
  }

  if (src == CMD_SOURCE_HTTP) {
    /* HTTP/WebApp config response — log it locally, don't send to MQTT.
     * The HTTP endpoint already returned OK to the web app. */
    const uint8_t *response_data = &payload[DATA_PACKET_HEADER_SIZE];
    uint16_t response_len = data_length;

    // Search for status marker (CFBL:/CFLR:/CFZB:/CFRS: prefix)
    const uint8_t *marker = (const uint8_t *)strstr((const char *)response_data, "CFBL:");
    if (marker == NULL) marker = (const uint8_t *)strstr((const char *)response_data, "CFLR:");
    if (marker == NULL) marker = (const uint8_t *)strstr((const char *)response_data, "CFZB:");
    if (marker == NULL) marker = (const uint8_t *)strstr((const char *)response_data, "CFRS:");
    if (marker == NULL) marker = (const uint8_t *)strstr((const char *)response_data, "BR:");

    if (marker != NULL) {
      size_t offset = marker - response_data;
      response_data = marker;
      response_len  = data_length - (uint16_t)offset;
    }

    ESP_LOGI(TAG, "HTTP WebApp config response (local only): %*s",
             response_len > 64 ? 64 : response_len, (char *)response_data);

    // ACK to LAN MCU
    downlink_send_ack_to_lan(ACK_TYPE_RECEIVED_OK,
                             g_internet_status == INTERNET_STATUS_ONLINE ? 1 : 0);
    ESP_LOGI(TAG, "HTTP response handled locally, ACK sent to LAN");
    return;
  }

  // Forward to server (MQTT) only if from server command
  if (g_internet_status == INTERNET_STATUS_ONLINE) {
    // Build uplink buffer: [handler_type(3)] + [data]
    uint16_t uplink_len = data_length + 3;
    uint8_t *uplink_buffer = (uint8_t *)malloc(uplink_len);
    if (uplink_buffer != NULL) {
      memcpy(uplink_buffer, handler_type, 3);
      memcpy(&uplink_buffer[3], &payload[DATA_PACKET_HEADER_SIZE], data_length);

      ESP_LOGI(TAG, "Uplink buffer (%u bytes):", uplink_len);
      ESP_LOG_BUFFER_HEXDUMP(TAG, uplink_buffer,
                             uplink_len > 64 ? 64 : uplink_len, ESP_LOG_INFO);

      ESP_LOGI(TAG, "Forwarding to MQTT server (server command response)");
      server_handler_enqueue_uplink(uplink_buffer, uplink_len);
      free(uplink_buffer);
    } else {
      ESP_LOGE(TAG, "Failed to allocate uplink buffer");
    }
    downlink_send_ack_to_lan(ACK_TYPE_RECEIVED_OK, 1);
  } else {
    downlink_send_ack_to_lan(ACK_TYPE_RECEIVED_OK, 0);
    ESP_LOGW(TAG, "Offline: LAN MCU will save to SD card");
  }
}

// ===== Process Data Query (DQ) - Send Pending Downlink =====
static void process_data_query(void) {
  bool served = false;

  // 0) FOTA trigger to LAN (highest priority)
  if (g_fota_trigger_pending) {
    // Build FOTA trigger packet: [CF][length(2)][CFFW]
    // Format matches what LAN expects for FOTA detection
    uint8_t fota_trigger_frame[8] = {
      (WAN_COMM_HEADER_CF >> 8) & 0xFF,  // 'C' = 0x43
      (WAN_COMM_HEADER_CF & 0xFF),        // 'F' = 0x46
      0x00, 0x04,                         // Length = 4 bytes
      'C', 'F', 'F', 'W'                  // FOTA marker
    };
    lan_comm_load_tx_data(g_lan_handle, fota_trigger_frame, sizeof(fota_trigger_frame));
    g_fota_trigger_pending = false;
    served = true;
    ESP_LOGI(TAG, "FOTA trigger (CFFW) sent to LAN MCU via DQ");
  }

  // 1) PC-side config scan: send CFCQ only after DQ
  if (!served && g_active_config_request_valid &&
      g_config_req_state == CONFIG_REQ_STATE_WAIT_DQ_FOR_CFCQ) {
    uint8_t config_request[4];
    config_request[0] = (WAN_COMM_HEADER_CF >> 8) & 0xFF;
    config_request[1] = WAN_COMM_HEADER_CF & 0xFF;
    config_request[2] = 'C';
    config_request[3] = 'Q';

    // Load CFCQ into TX
    lan_comm_load_tx_data(g_lan_handle, config_request, sizeof(config_request));
    g_config_req_state = CONFIG_REQ_STATE_WAIT_CONFIG_RESP;
    served = true;
    ESP_LOGI(TAG, "CFCQ command loaded to TX after DQ");
  }

  // 2) Config/FOTA downlink to LAN (from WAN config cache)
  if (!served && g_config_cache_has_config) {
    ESP_LOGI(TAG, "Sending cached config/FOTA to LAN MCU");
    downlink_handle_config_request();
    served = true;
  }

  // 3) Normal downlink data (DT) to LAN
  if (!served && g_pending_downlink_valid) {
    send_downlink_to_lan(&g_pending_downlink);
    g_pending_downlink_valid = false;
    served = true;
  }

  if (served) {
    // Clear GPIO handshake once something has been served
    clear_data_ready();
  } else {
    ESP_LOGI(TAG, "DQ received but nothing pending to send");
  }
}

// ===== Send Downlink to LAN with ACK Wait + Retry =====
static void send_downlink_to_lan(const downlink_item_t *item) {
  // Build data packet: [DT][handler_type(3)][length(2)][payload]
  uint16_t packet_size = DATA_PACKET_HEADER_SIZE + item->length;
  uint8_t *packet = (uint8_t *)malloc(packet_size);
  if (packet == NULL) {
    ESP_LOGE(TAG, "Failed to allocate downlink packet");
    return;
  }

  // Build packet
  packet[0] = (WAN_COMM_HEADER_DT >> 8) & 0xFF;
  packet[1] = WAN_COMM_HEADER_DT & 0xFF;
  const char *type_str = handler_id_to_string(item->target_id);
  memcpy(&packet[2], type_str, 3);
  packet[5] = (item->length >> 8) & 0xFF;
  packet[6] = item->length & 0xFF;
  memcpy(&packet[DATA_PACKET_HEADER_SIZE], item->data, item->length);

  // Load TX buffer
  lan_comm_load_tx_data(g_lan_handle, packet, packet_size);
  ESP_LOGI(TAG, "Downlink loaded: handler=%s, %u bytes", type_str, packet_size);

  // Wait for ACK from LAN MCU (with retry)
  for (int retry = 0; retry < MAX_RETRY_COUNT; retry++) {
    lan_comm_queue_receive(g_lan_handle);

    lan_comm_packet_t ack_packet;
    lan_comm_status_t status = lan_comm_get_received_packet(
        g_lan_handle, &ack_packet, ACK_WAIT_TIMEOUT_MS);

    if (status == LAN_COMM_OK && ack_packet.payload_length >= 2) {
      if (ack_packet.payload[0] == FRAME_TYPE_ACK &&
          ack_packet.payload[1] == ACK_TYPE_RECEIVED_OK) {
        ESP_LOGI(TAG, "Downlink ACK received from LAN");
        free(packet);
        return;
      }
    }

    ESP_LOGW(TAG, "Downlink ACK timeout, retry %d/%d", retry + 1,
             MAX_RETRY_COUNT);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGE(TAG, "Downlink send failed after max retries");
  free(packet);
}

// ===== Process Config Query from LAN =====
static void process_config_query(const lan_comm_packet_t *packet) {
  if (!g_active_config_request_valid || !g_active_config_request_ptr ||
      g_config_req_state != CONFIG_REQ_STATE_WAIT_CONFIG_RESP) {
    ESP_LOGW(TAG, "Config query received but no active request");
    return;
  }

  if (packet->payload_length < 6) {
    ESP_LOGE(TAG, "Config query packet too short");
    return;
  }

  // Extract config length from correct offset: [CF][CQ][length(2)][data...]
  //                                              0  2   4         6
  uint16_t config_len = (packet->payload[4] << 8) | packet->payload[5];
  if (config_len > g_active_config_request_ptr->buffer_size - 1) {
    config_len = g_active_config_request_ptr->buffer_size - 1;
  }

  // Copy config data to response buffer (starts at offset 6)
  memcpy(g_active_config_request_ptr->response_buffer, &packet->payload[6],
         config_len);
  g_active_config_request_ptr->response_buffer[config_len] = '\0';
  *g_active_config_request_ptr->response_len = config_len;

  // Update result and signal completion
  g_active_config_request_ptr->result = ESP_OK;
  ESP_LOGI(TAG, "LAN config response processed (%u bytes)", config_len);

  if (g_active_config_request_ptr->completion_sem) {
    xSemaphoreGive(g_active_config_request_ptr->completion_sem);
  }

  // Reset state
  g_active_config_request_valid = false;
  g_active_config_request_ptr = NULL;
  g_config_req_state = CONFIG_REQ_STATE_IDLE;
}

// ===== FOTA Handshake ACK Wait =====
static void fota_task(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(g_fota_start_sem, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    ESP_LOGI(TAG, "FOTA pre-handshake completed, initiating WAN update");
    server_connect_stop(g_server_type);
    led_show_blue();
    vTaskDelay(pdMS_TO_TICKS(5000));
    fota_handler_task_start();
    vTaskDelay(pdMS_TO_TICKS(200000));
  }
}

// ===== Send FOTA Trigger Config to LAN =====
// (WAN sends this when mismatch detected, to coordinate FOTA timing)
static void send_fota_trigger_to_lan(void) {
  // Set flag and signal GPIO - actual frame sent when LAN sends DQ
  g_fota_trigger_pending = true;
  gpio_set_level(GPIO_DATA_READY_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(1));
  gpio_set_level(GPIO_DATA_READY_PIN, 1);
  ESP_LOGI(TAG, "FOTA trigger queued, GPIO signaled - waiting for LAN DQ");
}

// ===== Public API: Enqueue Uplink to Server =====
bool server_handler_enqueue_uplink(const uint8_t *data, uint16_t len) {
  if (data == NULL || len == 0) {
    return false;
  }

  switch (g_server_type) {
  case CONFIG_SERVERTYPE_MQTT:
    return mqtt_enqueue_telemetry(data, len);
  case CONFIG_SERVERTYPE_HTTP:
    return http_enqueue_telemetry(data, len);
  case CONFIG_SERVERTYPE_COAP:
    return coap_enqueue_telemetry(data, len);
  default:
    return mqtt_enqueue_telemetry(data, len);
  }
}

// ===== Public API: Get Cached LAN FW Version =====
uint32_t mcu_lan_handler_get_lan_fw_version(void) {
  return g_cached_lan_fw_version;
}

// ===== Public API: Get RTC (Thread-safe) =====
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
