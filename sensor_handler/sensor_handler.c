#include "sensor_handler.h"
#include "mqtt_handler.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "sensor_handler"

// I2C Configuration for ESP32-P4 WiFi Dev Kit
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SDA_IO      7        // GPIO7 for SDA
#define I2C_MASTER_SCL_IO      8        // GPIO8 for SCL
#define I2C_MASTER_FREQ_HZ     400000   // 400kHz
#define I2C_MASTER_TIMEOUT_MS  1000

// HTU21D Sensor Commands
#define HTU21_ADDR             0x40     // I2C address
#define HTU21_TEMP_CMD         0xE3     // Trigger Temperature Measurement (Hold Master)
#define HTU21_HUMID_CMD        0xE5     // Trigger Humidity Measurement (Hold Master)
#define HTU21_SOFT_RESET       0xFE     // Soft Reset

// Global variables for sensor data
static int16_t s_temp_x100 = 0;
static int16_t s_humid_x100 = 0;

/**
 * @brief Read temperature from HTU21D sensor
 */
static esp_err_t htu21_read_temperature(i2c_master_dev_handle_t dev_handle, int16_t *temp_x100)
{
    uint8_t cmd = HTU21_TEMP_CMD;
    uint8_t data[3];
    esp_err_t ret;

    // Send temperature measurement command
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send temperature command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for measurement completion (max 50ms for 14-bit resolution)
    vTaskDelay(pdMS_TO_TICKS(50));

    // Read 3 bytes: MSB, LSB, CRC
    ret = i2c_master_receive(dev_handle, data, 3, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Calculate temperature: T = -46.85 + 175.72 * (S_T / 2^16)
    // Clear status bits (last 2 bits)
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    raw &= 0xFFFC;

    // Calculate temperature in 0.01°C units to avoid float
    int32_t temp = -4685 + (17572L * raw) / 65536;
    *temp_x100 = (int16_t)temp;

    return ESP_OK;
}

/**
 * @brief Read humidity from HTU21D sensor
 */
static esp_err_t htu21_read_humidity(i2c_master_dev_handle_t dev_handle, int16_t *humid_x100)
{
    uint8_t cmd = HTU21_HUMID_CMD;
    uint8_t data[3];
    esp_err_t ret;

    // Send humidity measurement command
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send humidity command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for measurement completion (max 16ms for 12-bit resolution)
    vTaskDelay(pdMS_TO_TICKS(20));

    // Read 3 bytes: MSB, LSB, CRC
    ret = i2c_master_receive(dev_handle, data, 3, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read humidity data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Calculate humidity: RH = -6 + 125 * (S_RH / 2^16)
    // Clear status bits (last 2 bits)
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    raw &= 0xFFFC;

    // Calculate humidity in 0.01% units to avoid float
    int32_t humid = -600 + (12500L * raw) / 65536;
    *humid_x100 = (int16_t)humid;

    return ESP_OK;
}

/**
 * @brief Initialize I2C master bus and HTU21D device
 */
static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle, 
                                 i2c_master_dev_handle_t *dev_handle)
{
    // Configure I2C master bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, bus_handle));
    ESP_LOGI(TAG, "I2C master bus initialized on SDA=%d, SCL=%d", 
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    // Configure HTU21D device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    ESP_ERROR_CHECK(i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle));
    ESP_LOGI(TAG, "HTU21D device added to I2C bus (address: 0x%02X)", HTU21_ADDR);

    return ESP_OK;
}

/**
 * @brief Scan I2C bus for devices
 */
static void i2c_scanner(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int devices_found = 0;
    
    for (uint8_t addr = 1; addr < 127; addr++) {
        esp_err_t ret = i2c_master_probe(bus_handle, addr, 1000);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Device found at address 0x%02X", addr);
            devices_found++;
        }
    }
    
    if (devices_found == 0) {
        ESP_LOGE(TAG, "No I2C devices found! Check wiring and pull-ups.");
    } else {
        ESP_LOGI(TAG, "Scan complete. Found %d device(s)", devices_found);
    }
}


/**
 * @brief Perform soft reset of HTU21D sensor
 */
static esp_err_t htu21_soft_reset(i2c_master_dev_handle_t dev_handle)
{
    uint8_t cmd = HTU21_SOFT_RESET;
    esp_err_t ret;

    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send soft reset command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for sensor to complete reset (15ms max)
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "HTU21D soft reset completed");

    return ESP_OK;
}

/**
 * @brief Sensor task - reads HTU21D and publishes data
 */
static void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;

    // Initialize I2C and HTU21D
    if (i2c_master_init(&bus_handle, &dev_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        vTaskDelete(NULL);
        return;
    }
    // Try to initialize just the bus for scanning
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    if (i2c_new_master_bus(&bus_config, &bus_handle) == ESP_OK) {
        i2c_scanner(bus_handle);
    }

    // Perform soft reset on startup
    htu21_soft_reset(dev_handle);

    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        // Read temperature
        if (htu21_read_temperature(dev_handle, &s_temp_x100) == ESP_OK) {
            ESP_LOGI(TAG, "Temperature: %d.%02d °C", 
                     s_temp_x100 / 100, abs(s_temp_x100 % 100));
        } else {
            ESP_LOGE(TAG, "Failed to read temperature");
        }

        // Small delay between measurements
        vTaskDelay(pdMS_TO_TICKS(100));

        // Read humidity
        if (htu21_read_humidity(dev_handle, &s_humid_x100) == ESP_OK) {
            ESP_LOGI(TAG, "Humidity: %d.%02d %%", 
                     s_humid_x100 / 100, abs(s_humid_x100 % 100));
        } else {
            ESP_LOGE(TAG, "Failed to read humidity");
        }

        // Build telemetry JSON payload
        char telemetry[128];
        snprintf(telemetry, sizeof(telemetry),
                 "{\"temp\":%d.%02d,\"humid\":%d.%02d}",
                 s_temp_x100 / 100, abs(s_temp_x100 % 100),
                 s_humid_x100 / 100, abs(s_humid_x100 % 100));

        ESP_LOGI(TAG, "Telemetry: %s", telemetry);
        
        // Send to MQTT handler
        mqtt_build_telemetry_payload(telemetry, strlen(telemetry));

        // Read every 5 seconds
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/**
 * @brief Start the sensor handler task
 */
void sensor_handler_start(void)
{
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Sensor handler started");
}

/**
 * @brief Get current temperature value
 */
int16_t sensor_get_temp_x100(void)
{
    return s_temp_x100;
}

/**
 * @brief Get current humidity value
 */
int16_t sensor_get_humid_x100(void)
{
    return s_humid_x100;
}
