/**
 * @file mcu_lan_handler.c
 * @brief MCU LAN Communication Handler (Slave)
 * 
 * MCU WAN: Handles wide area network (WiFi/4G/Ethernet) and server communication (MQTT)
 * Has RTC for timestamping
 * Acts as SPI Slave to receive commands from MCU LAN (Master)
 */

#include "mcu_lan_handler.h"
#include "lan_comm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <time.h>

static const char *TAG = "MCU_LAN_HANDLER";

// ===== Configuration =====
#define MCU_LAN_TASK_STACK_SIZE 4096
#define MCU_LAN_TASK_PRIORITY 5
#define PERIODIC_RTC_UPDATE_MS 1000
#define SERVER_QUEUE_SIZE 30

// ===== Protocol Commands =====
#define CMD_HANDSHAKE_ACK 0x01
#define CMD_REQUEST_RTC_CONFIG 0x02
#define CMD_FOTA_START 0x03

// ===== State Machine =====
typedef enum {
    STATE_INIT,
    STATE_HANDSHAKE,
    STATE_DATA_MODE,
    STATE_ERROR
} lan_handler_state_t;

typedef enum {
    INTERNET_STATUS_OFFLINE = 0,
    INTERNET_STATUS_ONLINE = 1
} internet_status_t;

// ===== Data Structures =====
typedef struct {
    uint8_t *data;
    uint16_t length;
    uint8_t rtc_timestamp[14];  // dd/mm/yyyy-hh:mm:ss
} lan_data_packet_t;

typedef struct {
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_time_t;

typedef struct {
    bool has_config;
    bool is_fota;
    uint8_t *config_data;
    uint16_t config_length;
} config_data_t;

// ===== Global Variables =====
static lan_comm_handle_t g_lan_handle = NULL;  // SPI Slave handle
static TaskHandle_t g_task_handle = NULL;
static QueueHandle_t g_server_queue = NULL;  // Queue to server handler
static lan_handler_state_t g_state = STATE_INIT;
static bool g_handler_running = false;
static internet_status_t g_internet_status = INTERNET_STATUS_OFFLINE;
static config_data_t g_config_cache = {0};
static esp_timer_handle_t g_periodic_timer = NULL;

// ===== Forward Declarations =====
static void mcu_lan_handler_task(void *pvParameters);
static esp_err_t perform_handshake_slave(void);
static esp_err_t handle_received_data(const uint8_t *payload, uint16_t length);
static esp_err_t send_rtc_config_net_status(void);
static void periodic_rtc_callback(void *arg);
static void get_rtc_time(rtc_time_t *rtc);
static void send_ack_to_lan(void);

// ===== Initialization =====
esp_err_t mcu_lan_handler_start(void) {
    if (g_handler_running) {
        ESP_LOGW(TAG, "Handler already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting MCU LAN Handler (Slave with RTC)");

    // Initialize LAN communication library (Slave mode)
    lan_comm_config_t lan_config = {
        .gpio_sck = 12,
        .gpio_cs = 10,
        .gpio_io0 = 11,  // MOSI
        .gpio_io1 = 13,  // MISO
        .gpio_io2 = -1,
        .gpio_io3 = -1,
        .mode = 0,
        .host_id = SPI2_HOST,
        .dma_channel = SPI_DMA_CH_AUTO,
        .rx_buffer_size = 4096,
        .tx_buffer_size = 4096,
        .enable_quad_mode = false
    };

    lan_comm_status_t status = lan_comm_init(&lan_config, &g_lan_handle);
    if (status != LAN_COMM_OK) {
        ESP_LOGE(TAG, "Failed to initialize LAN comm: %d", status);
        return ESP_FAIL;
    }

    // Create queue for forwarding to server handler
    g_server_queue = xQueueCreate(SERVER_QUEUE_SIZE, sizeof(lan_data_packet_t));
    if (g_server_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create server queue");
        lan_comm_deinit(g_lan_handle);
        return ESP_FAIL;
    }

    // TODO: Initialize RTC hardware
    ESP_LOGI(TAG, "TODO: Initialize RTC hardware (DS1307 or similar)");

    // Create periodic timer for RTC updates (optional - can be sync'd via NTP)
    const esp_timer_create_args_t timer_args = {
        .callback = periodic_rtc_callback,
        .name = "rtc_update"
    };
    esp_timer_create(&timer_args, &g_periodic_timer);

    // Create handler task
    BaseType_t ret = xTaskCreate(
        mcu_lan_handler_task,
        "mcu_lan_task",
        MCU_LAN_TASK_STACK_SIZE,
        NULL,
        MCU_LAN_TASK_PRIORITY,
        &g_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        vQueueDelete(g_server_queue);
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

    if (g_periodic_timer != NULL) {
        esp_timer_stop(g_periodic_timer);
        esp_timer_delete(g_periodic_timer);
        g_periodic_timer = NULL;
    }

    if (g_task_handle != NULL) {
        vTaskDelete(g_task_handle);
        g_task_handle = NULL;
    }

    if (g_server_queue != NULL) {
        vQueueDelete(g_server_queue);
        g_server_queue = NULL;
    }

    if (g_lan_handle != NULL) {
        lan_comm_deinit(g_lan_handle);
        g_lan_handle = NULL;
    }

    if (g_config_cache.config_data != NULL) {
        free(g_config_cache.config_data);
        g_config_cache.config_data = NULL;
    }

    // TODO: Deinitialize RTC

    ESP_LOGI(TAG, "MCU LAN Handler stopped");
    return ESP_OK;
}

// ===== Main Task =====
static void mcu_lan_handler_task(void *pvParameters) {
    ESP_LOGI(TAG, "MCU LAN Handler task started");

    // Phase 1: Handshake
    g_state = STATE_HANDSHAKE;
    while (g_handler_running && g_state == STATE_HANDSHAKE) {
        esp_err_t ret = perform_handshake_slave();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Handshake successful, entering DATA_MODE");
            g_state = STATE_DATA_MODE;
            
            // Start periodic timer (optional for RTC sync)
            esp_timer_start_periodic(g_periodic_timer, PERIODIC_RTC_UPDATE_MS * 1000);
            break;
        }
        ESP_LOGD(TAG, "Waiting for handshake from LAN MCU...");
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Phase 2: Main Data Reception Loop
    while (g_handler_running && g_state == STATE_DATA_MODE) {
        // Queue receive transaction
        lan_comm_queue_receive(g_lan_handle);
        
        // Wait for data from LAN MCU (Master)
        lan_comm_packet_t packet;
        lan_comm_status_t status = lan_comm_get_received_packet(
            g_lan_handle,
            &packet,
            100  // 100ms timeout
        );

        if (status == LAN_COMM_OK) {
            if (packet.header_type == LAN_COMM_HEADER_DT) {
                // Standard telemetry data received
                ESP_LOGI(TAG, "Received data packet (%d bytes)", packet.payload_length);
                handle_received_data(packet.payload, packet.payload_length);
                
                // Send ACK back to LAN MCU
                send_ack_to_lan();
                
            } else if (packet.header_type == LAN_COMM_HEADER_CF) {
                // Command received
                uint8_t cmd = packet.payload[0];
                
                if (cmd == CMD_REQUEST_RTC_CONFIG) {
                    ESP_LOGI(TAG, "Received request for RTC/Config/NetStatus");
                    send_rtc_config_net_status();
                    
                } else if (cmd == CMD_FOTA_START) {
                    ESP_LOGI(TAG, "Received FOTA command");
                    // TODO: Notify config task to start FOTA
                    send_ack_to_lan();
                    
                } else {
                    ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
                }
            }
        } else if (status != LAN_COMM_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Error receiving packet: %d", status);
        }
    }

    ESP_LOGI(TAG, "MCU LAN Handler task exiting");
    vTaskDelete(NULL);
}

// ===== Handshake Implementation (Slave) =====
static esp_err_t perform_handshake_slave(void) {
    // Queue receive transaction
    lan_comm_queue_receive(g_lan_handle);
    
    // Wait for ACK from LAN MCU (Master)
    lan_comm_packet_t packet;
    lan_comm_status_t status = lan_comm_get_received_packet(
        g_lan_handle,
        &packet,
        100  // 100ms timeout
    );

    if (status == LAN_COMM_OK) {
        if (packet.header_type == LAN_COMM_HEADER_CF && 
            packet.payload[0] == CMD_HANDSHAKE_ACK) {
            
            ESP_LOGI(TAG, "Received handshake ACK from LAN MCU");
            
            // Send ACK back
            uint8_t ack_response[3] = {
                (LAN_COMM_HEADER_CF >> 8) & 0xFF,
                LAN_COMM_HEADER_CF & 0xFF,
                CMD_HANDSHAKE_ACK
            };
            
            lan_comm_load_tx_data(g_lan_handle, ack_response, sizeof(ack_response));
            
            return ESP_OK;
        }
    }
    
    return ESP_FAIL;
}

// ===== Data Handling =====
static esp_err_t handle_received_data(const uint8_t *payload, uint16_t length) {
    if (payload == NULL || length < 19) {  // At least RTC timestamp
        return ESP_ERR_INVALID_ARG;
    }

    // Extract RTC timestamp (first 14 bytes: dd/mm/yyyy-hh:mm:ss)
    uint8_t rtc_str[20] = {0};
    memcpy(rtc_str, payload, 19);
    
    ESP_LOGI(TAG, "Data with RTC: %s", (char *)rtc_str);

    // Extract actual data (after RTC)
    const uint8_t *data = &payload[19];
    uint16_t data_length = length - 19;

    // Check internet status before forwarding
    if (g_internet_status == INTERNET_STATUS_ONLINE) {
        // Forward to server handler
        lan_data_packet_t server_packet;
        server_packet.data = (uint8_t *)malloc(data_length);
        if (server_packet.data != NULL) {
            memcpy(server_packet.data, data, data_length);
            server_packet.length = data_length;
            memcpy(server_packet.rtc_timestamp, rtc_str, 14);
            
            if (xQueueSend(g_server_queue, &server_packet, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "Data forwarded to server handler (%d bytes)", data_length);
            } else {
                ESP_LOGE(TAG, "Server queue full, dropping data");
                free(server_packet.data);
            }
        }
    } else {
        ESP_LOGW(TAG, "No internet, cannot forward data to server");
        // Note: MCU LAN will save to SD card if it doesn't receive ACK
    }

    return ESP_OK;
}

// ===== Send RTC, Config, and Network Status =====
static esp_err_t send_rtc_config_net_status(void) {
    uint8_t response_buffer[512] = {0};
    uint16_t offset = 0;

    // Header
    response_buffer[offset++] = (LAN_COMM_HEADER_CF >> 8) & 0xFF;
    response_buffer[offset++] = LAN_COMM_HEADER_CF & 0xFF;
    response_buffer[offset++] = CMD_REQUEST_RTC_CONFIG;

    // Get current RTC time
    rtc_time_t rtc;
    get_rtc_time(&rtc);

    // RTC time (19 bytes: dd/mm/yyyy-hh:mm:ss)
    offset += snprintf((char *)&response_buffer[offset], 20, 
                      "%02d/%02d/%04d-%02d:%02d:%02d",
                      rtc.day, rtc.month, rtc.year,
                      rtc.hour, rtc.minute, rtc.second);

    // Config data
    if (g_config_cache.has_config) {
        response_buffer[offset++] = 0x01;  // Has config
        if (g_config_cache.is_fota) {
            response_buffer[offset++] = 0xFF;  // FOTA flag
        } else {
            response_buffer[offset++] = 0x00;  // Normal config
        }
        memcpy(&response_buffer[offset], g_config_cache.config_data, 
               g_config_cache.config_length);
        offset += g_config_cache.config_length;
    } else {
        response_buffer[offset++] = 0x00;  // No config
    }

    // Internet status
    response_buffer[offset++] = (g_internet_status == INTERNET_STATUS_ONLINE) ? 0x01 : 0x00;

    // Load TX data
    lan_comm_load_tx_data(g_lan_handle, response_buffer, offset);

    ESP_LOGI(TAG, "Sent RTC/Config/NetStatus response (%d bytes)", offset);
    return ESP_OK;
}

// ===== Send ACK =====
static void send_ack_to_lan(void) {
    uint8_t ack_response[3] = {
        (LAN_COMM_HEADER_CF >> 8) & 0xFF,
        LAN_COMM_HEADER_CF & 0xFF,
        CMD_HANDSHAKE_ACK  // Use as generic ACK
    };
    
    lan_comm_load_tx_data(g_lan_handle, ack_response, sizeof(ack_response));
    ESP_LOGD(TAG, "ACK sent to LAN MCU");
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

static void periodic_rtc_callback(void *arg) {
    // Optional: Sync RTC with NTP or update system time from RTC
    ESP_LOGD(TAG, "Periodic RTC update (placeholder)");
    
    // TODO: Sync RTC with NTP if internet is available
    /*
     * if (g_internet_status == INTERNET_STATUS_ONLINE) {
     *     ntp_sync_rtc();
     * }
     */
}

// ===== Internet Status Management =====
void mcu_lan_handler_set_internet_status(internet_status_t status) {
    if (g_internet_status != status) {
        g_internet_status = status;
        ESP_LOGI(TAG, "Internet status changed to: %s", 
                 status == INTERNET_STATUS_ONLINE ? "ONLINE" : "OFFLINE");
    }
}

internet_status_t mcu_lan_handler_get_internet_status(void) {
    return g_internet_status;
}

// ===== Config Update (Called by Config Task) =====
void mcu_lan_handler_update_config(const uint8_t *config_data, uint16_t length, bool is_fota) {
    if (g_config_cache.config_data != NULL) {
        free(g_config_cache.config_data);
    }

    g_config_cache.config_data = (uint8_t *)malloc(length);
    if (g_config_cache.config_data != NULL) {
        memcpy(g_config_cache.config_data, config_data, length);
        g_config_cache.config_length = length;
        g_config_cache.has_config = true;
        g_config_cache.is_fota = is_fota;
        ESP_LOGI(TAG, "Config cache updated (%d bytes, FOTA: %s)", 
                 length, is_fota ? "YES" : "NO");
    }
}

// ===== Get Server Queue (for server handler to dequeue) =====
QueueHandle_t mcu_lan_handler_get_server_queue(void) {
    return g_server_queue;
}
