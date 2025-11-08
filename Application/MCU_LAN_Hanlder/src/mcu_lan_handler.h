/**
 * @file mcu_lan_handler.c
 * @brief MCU LAN Communication Handler Implementation
 */

#include "mcu_lan_handler.h"
#include "config_handler.h"
#include "lan_comm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "mcu_lan_handler";

// LAN communication handle (private to this module)
static lan_comm_handle_t g_lan_comm_handle = NULL;

// Handler task control
static bool mcu_lan_handler_running = false;
static TaskHandle_t mcu_lan_handler_task_handle = NULL;

// Initialization flag - ensures init happens ONLY ONCE in lifetime
static bool g_initialized = false;

// Configuration for SPI pins (adjust according to your hardware)
#define MCU_LAN_SPI_SCK     GPIO_NUM_9
#define MCU_LAN_SPI_CS      GPIO_NUM_10
#define MCU_LAN_SPI_MOSI    GPIO_NUM_12
#define MCU_LAN_SPI_MISO    GPIO_NUM_11
#define MCU_LAN_SPI_WP      GPIO_NUM_13 // For Quad mode
#define MCU_LAN_SPI_HD      GPIO_NUM_14 // For Quad mode

#define MCU_LAN_ACK_TIMEOUT_MS 1000

/**
 * @brief Error callback
 */
static void mcu_lan_error_cb(lan_comm_status_t error, const char *context, void *user_data)
{
    ESP_LOGE(TAG, "LAN comm error: %d - %s", error, context);
}

/**
 * @brief MCU LAN handler task
 */
static void mcu_lan_handler_task(void *arg)
{
    mcu_lan_config_data_t lan_config;
    uint8_t ack_buffer[64];

    ESP_LOGI(TAG, "MCU LAN handler task started");

    while (mcu_lan_handler_running)
    {
        // Wait for command from queue (sent by config_handler)
        if (xQueueReceive(g_mcu_lan_config_queue, &lan_config, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            ESP_LOGI(TAG, "Received command to send to LAN MCU: %d bytes", lan_config.length);
            ESP_LOG_BUFFER_HEXDUMP(TAG, (uint8_t *)lan_config.command, lan_config.length, ESP_LOG_DEBUG);

            // Validate command length
            if (lan_config.length <= 0 || lan_config.length > sizeof(lan_config.command))
            {
                ESP_LOGW(TAG, "Invalid command length: %d", lan_config.length);
                continue;
            }

            // Send command to LAN MCU
            lan_comm_status_t status = lan_comm_send_command(g_lan_comm_handle,
                                                             (uint8_t *)lan_config.command,
                                                             lan_config.length);

            if (status == LAN_COMM_OK)
            {
                ESP_LOGI(TAG, "Command sent to LAN MCU successfully");

                // Wait for ACK from LAN MCU
                vTaskDelay(pdMS_TO_TICKS(50)); // Small delay for LAN MCU to process

                status = lan_comm_request_data(g_lan_comm_handle,
                                               ack_buffer,
                                               sizeof(ack_buffer));

                if (status == LAN_COMM_OK)
                {
                    // Check ACK response
                    if (strncmp((char *)ack_buffer, "ACK", 3) == 0 ||
                        strncmp((char *)ack_buffer, "CMD_ACK", 7) == 0)
                    {
                        ESP_LOGI(TAG, "Received ACK from LAN MCU");
                    }
                    else if (strncmp((char *)ack_buffer, "NACK", 4) == 0)
                    {
                        ESP_LOGW(TAG, "Received NACK from LAN MCU");
                        ESP_LOG_BUFFER_HEXDUMP(TAG, ack_buffer, 32, ESP_LOG_WARN);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Unexpected response from LAN MCU");
                        ESP_LOG_BUFFER_HEXDUMP(TAG, ack_buffer, 32, ESP_LOG_WARN);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to receive ACK from LAN MCU: %d", status);
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to send command to LAN MCU: %d", status);
            }
        }
    }

    ESP_LOGI(TAG, "MCU LAN handler task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Initialize MCU LAN handler (STATIC - called only once in lifetime)
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t mcu_lan_handler_init(void)
{
    // Check if already initialized (init only once in lifetime)
    if (g_initialized)
    {
        ESP_LOGI(TAG, "MCU LAN handler already initialized (lifetime init)");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MCU LAN handler (first and only time)");

    // Verify that config handler queue exists
    if (!g_mcu_lan_config_queue)
    {
        ESP_LOGE(TAG, "MCU LAN config queue not initialized. Start config_handler first!");
        return ESP_FAIL;
    }

    // Configure LAN communication (Master mode)
    lan_comm_config_t lan_config = {
        .gpio_sck = MCU_LAN_SPI_SCK,
        .gpio_cs = MCU_LAN_SPI_CS,
        .gpio_io0 = MCU_LAN_SPI_MOSI,
        .gpio_io1 = MCU_LAN_SPI_MISO,
        .gpio_io2 = MCU_LAN_SPI_WP,
        .gpio_io3 = MCU_LAN_SPI_HD,

        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz
        .mode = 0,                          // SPI Mode 0
        .host_id = SPI2_HOST,

        .dma_channel = SPI_DMA_CH_AUTO,
        .queue_size = 7,

        .transfer_callback = NULL,
        .error_callback = mcu_lan_error_cb,
        .user_data = NULL,

        .enable_quad_mode = false};

    // Initialize LAN communication library
    lan_comm_status_t status = lan_comm_init(&lan_config, &g_lan_comm_handle);
    if (status != LAN_COMM_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LAN communication: %d", status);
        return ESP_FAIL;
    }

    // Set blocking mode for simpler task implementation
    lan_comm_set_blocking_mode(g_lan_comm_handle, true);

    // Mark as initialized (will never initialize again)
    g_initialized = true;

    ESP_LOGI(TAG, "MCU LAN handler initialized successfully (lifetime init complete)");

    return ESP_OK;
}

/**
 * @brief Start MCU LAN handler task
 *
 */
esp_err_t mcu_lan_handler_start(void)
{
    // Initialize if not already done (init only once)
    if (!g_initialized)
    {
        esp_err_t ret = mcu_lan_handler_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize MCU LAN handler");
            return ret;
        }
    }

    // Check if task already running
    if (mcu_lan_handler_running)
    {
        ESP_LOGW(TAG, "MCU LAN handler task already running");
        return ESP_OK;
    }

    // Check if initialized
    if (g_lan_comm_handle == NULL)
    {
        ESP_LOGE(TAG, "MCU LAN handler not initialized (internal error)");
        return ESP_FAIL;
    }

    mcu_lan_handler_running = true;

    BaseType_t ret = xTaskCreate(
        mcu_lan_handler_task,
        "mcu_lan_handler",
        4096,
        NULL,
        5, // Priority
        &mcu_lan_handler_task_handle);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create MCU LAN handler task");
        mcu_lan_handler_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MCU LAN handler task started");

    return ESP_OK;
}

/**
 * @brief Stop MCU LAN handler task
 */
esp_err_t mcu_lan_handler_stop(void)
{
    if (!mcu_lan_handler_running)
    {
        ESP_LOGD(TAG, "MCU LAN handler task not running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping MCU LAN handler task");

    mcu_lan_handler_running = false;

    // Wait for task to exit gracefully
    if (mcu_lan_handler_task_handle)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        mcu_lan_handler_task_handle = NULL;
    }

    ESP_LOGI(TAG, "MCU LAN handler task stopped");

    return ESP_OK;
}
