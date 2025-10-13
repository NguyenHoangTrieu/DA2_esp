/*
 * HTU21D Temperature and Humidity Sensor Example
 * 
 * This example shows how to read temperature and humidity from HTU21D sensor
 * using ESP-IDF I2C master driver on ESP32-P4
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "htu21d_example";

// I2C Configuration for ESP32-P4 WiFi Dev Kit
#define I2C_MASTER_SCL_IO           8               /*!< GPIO number for I2C master clock */
#define I2C_MASTER_SDA_IO           7               /*!< GPIO number for I2C master data  */
#define I2C_MASTER_NUM              I2C_NUM_0       /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ          400000          /*!< I2C master clock frequency (100kHz) */
#define I2C_MASTER_TIMEOUT_MS       1000

// HTU21D Sensor Configuration
#define HTU21D_SENSOR_ADDR          0x40            /*!< I2C address of HTU21D sensor */
#define HTU21D_TEMP_HOLD_CMD        0xE3            /*!< Trigger Temp Measurement (Hold Master) */
#define HTU21D_HUMID_HOLD_CMD       0xE5            /*!< Trigger RH Measurement (Hold Master) */
#define HTU21D_TEMP_NOHOLD_CMD      0xF3            /*!< Trigger Temp Measurement (No Hold Master) */
#define HTU21D_HUMID_NOHOLD_CMD     0xF5            /*!< Trigger RH Measurement (No Hold Master) */
#define HTU21D_SOFT_RESET           0xFE            /*!< Soft reset command */
#define HTU21D_READ_USER_REG        0xE7            /*!< Read user register */
#define HTU21D_WRITE_USER_REG       0xE6            /*!< Write user register */

/**
 * @brief Scan I2C bus for devices
 */
static void i2c_scanner(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int devices_found = 0;
    
    for (uint8_t addr = 1; addr < 127; addr++) {
        esp_err_t ret = i2c_master_probe(bus_handle, addr, I2C_MASTER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Device found at address 0x%02X", addr);
            devices_found++;
        }
    }
    
    if (devices_found == 0) {
        ESP_LOGE(TAG, "No I2C devices found!");
    } else {
        ESP_LOGI(TAG, "Found %d device(s)", devices_found);
    }
}

/**
 * @brief Soft reset HTU21D sensor
 */
static esp_err_t htu21d_soft_reset(i2c_master_dev_handle_t dev_handle)
{
    uint8_t cmd = HTU21D_SOFT_RESET;
    esp_err_t ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Soft reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(15)); // Wait 15ms after reset
    ESP_LOGI(TAG, "HTU21D soft reset completed");
    return ESP_OK;
}

/**
 * @brief Read temperature from HTU21D (Hold Master mode)
 * 
 * In Hold Master mode, the sensor holds the SCL line low during measurement.
 * This is simpler but blocks the I2C bus.
 */
static esp_err_t htu21d_read_temperature_hold(i2c_master_dev_handle_t dev_handle, float *temperature)
{
    uint8_t cmd = HTU21D_TEMP_HOLD_CMD;
    uint8_t data[3]; // MSB, LSB, CRC
    
    // Send command and read result in one operation
    // The sensor will hold SCL during measurement (~50ms)
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, data, 3, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Temperature read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Calculate temperature: T = -46.85 + 175.72 * (S_T / 2^16)
    uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    raw_temp &= 0xFFFC; // Clear status bits (last 2 bits)
    
    *temperature = -46.85 + 175.72 * ((float)raw_temp / 65536.0);
    
    return ESP_OK;
}

/**
 * @brief Read humidity from HTU21D (Hold Master mode)
 */
static esp_err_t htu21d_read_humidity_hold(i2c_master_dev_handle_t dev_handle, float *humidity)
{
    uint8_t cmd = HTU21D_HUMID_HOLD_CMD;
    uint8_t data[3]; // MSB, LSB, CRC
    
    // Send command and read result in one operation
    // The sensor will hold SCL during measurement (~16ms)
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, data, 3, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Humidity read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Calculate humidity: RH = -6 + 125 * (S_RH / 2^16)
    uint16_t raw_humid = ((uint16_t)data[0] << 8) | data[1];
    raw_humid &= 0xFFFC; // Clear status bits
    
    *humidity = -6.0 + 125.0 * ((float)raw_humid / 65536.0);
    
    // Clamp to valid range
    if (*humidity < 0) *humidity = 0;
    if (*humidity > 100) *humidity = 100;
    
    return ESP_OK;
}

/**
 * @brief Read temperature from HTU21D (No Hold Master mode)
 * 
 * In No Hold Master mode, the I2C bus is not blocked during measurement.
 * We need to poll the sensor until data is ready.
 */
static esp_err_t htu21d_read_temperature_nohold(i2c_master_dev_handle_t dev_handle, float *temperature)
{
    uint8_t cmd = HTU21D_TEMP_NOHOLD_CMD;
    uint8_t data[3];
    esp_err_t ret;
    
    // Send measurement trigger command
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send temperature command: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Poll for data ready (max 50ms for 14-bit temperature)
    int retry_count = 0;
    const int max_retries = 10;
    while (retry_count < max_retries) {
        vTaskDelay(pdMS_TO_TICKS(10)); // Wait 10ms between polls
        
        ret = i2c_master_receive(dev_handle, data, 3, I2C_MASTER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            // Data ready, calculate temperature
            uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
            raw_temp &= 0xFFFC;
            *temperature = -46.85 + 175.72 * ((float)raw_temp / 65536.0);
            return ESP_OK;
        }
        
        retry_count++;
    }
    
    ESP_LOGE(TAG, "Temperature measurement timeout");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Read humidity from HTU21D (No Hold Master mode)
 */
static esp_err_t htu21d_read_humidity_nohold(i2c_master_dev_handle_t dev_handle, float *humidity)
{
    uint8_t cmd = HTU21D_HUMID_NOHOLD_CMD;
    uint8_t data[3];
    esp_err_t ret;
    
    // Send measurement trigger command
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send humidity command: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Poll for data ready (max 16ms for 12-bit humidity)
    int retry_count = 0;
    const int max_retries = 5;
    while (retry_count < max_retries) {
        vTaskDelay(pdMS_TO_TICKS(5)); // Wait 5ms between polls
        
        ret = i2c_master_receive(dev_handle, data, 3, I2C_MASTER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            // Data ready, calculate humidity
            uint16_t raw_humid = ((uint16_t)data[0] << 8) | data[1];
            raw_humid &= 0xFFFC;
            *humidity = -6.0 + 125.0 * ((float)raw_humid / 65536.0);
            
            // Clamp to valid range
            if (*humidity < 0) *humidity = 0;
            if (*humidity > 100) *humidity = 100;
            return ESP_OK;
        }
        
        retry_count++;
    }
    
    ESP_LOGE(TAG, "Humidity measurement timeout");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Read user register from HTU21D
 */
static esp_err_t htu21d_read_user_register(i2c_master_dev_handle_t dev_handle, uint8_t *reg_value)
{
    uint8_t cmd = HTU21D_READ_USER_REG;
    return i2c_master_transmit_receive(dev_handle, &cmd, 1, reg_value, 1, I2C_MASTER_TIMEOUT_MS);
}

/**
 * @brief Initialize I2C master bus
 */
static void i2c_master_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle)
{
    // Initialize I2C bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    // Scan I2C bus
    i2c_scanner(*bus_handle);

    // Add HTU21D device to bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21D_SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle));
    ESP_LOGI(TAG, "HTU21D device added (address: 0x%02X)", HTU21D_SENSOR_ADDR);
}

void app_main(void)
{
    float temperature, humidity;
    uint8_t user_reg;
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    
    // Initialize I2C and HTU21D
    i2c_master_init(&bus_handle, &dev_handle);
    ESP_LOGI(TAG, "I2C initialized successfully");

    // Perform soft reset
    ESP_ERROR_CHECK(htu21d_soft_reset(dev_handle));

    // Read user register to verify communication
    ESP_ERROR_CHECK(htu21d_read_user_register(dev_handle, &user_reg));
    ESP_LOGI(TAG, "HTU21D User Register = 0x%02X", user_reg);

    ESP_LOGI(TAG, "\n=== Hold Master Mode Demo ===");
    
    // Read temperature using Hold Master mode
    ESP_ERROR_CHECK(htu21d_read_temperature_hold(dev_handle, &temperature));
    ESP_LOGI(TAG, "Temperature (Hold): %.2f °C", temperature);

    // Read humidity using Hold Master mode
    ESP_ERROR_CHECK(htu21d_read_humidity_hold(dev_handle, &humidity));
    ESP_LOGI(TAG, "Humidity (Hold): %.2f %%", humidity);

    ESP_LOGI(TAG, "\n=== No Hold Master Mode Demo ===");
    
    // Read temperature using No Hold Master mode
    ESP_ERROR_CHECK(htu21d_read_temperature_nohold(dev_handle, &temperature));
    ESP_LOGI(TAG, "Temperature (No Hold): %.2f °C", temperature);

    // Read humidity using No Hold Master mode
    ESP_ERROR_CHECK(htu21d_read_humidity_nohold(dev_handle, &humidity));
    ESP_LOGI(TAG, "Humidity (No Hold): %.2f %%", humidity);

    ESP_LOGI(TAG, "\n=== Continuous Reading Loop ===");
    
    // Continuous reading loop
    while (1) {
        // Read both values using Hold Master mode (simpler)
        if (htu21d_read_temperature_hold(dev_handle, &temperature) == ESP_OK &&
            htu21d_read_humidity_hold(dev_handle, &humidity) == ESP_OK) {
            ESP_LOGI(TAG, "Temp: %.2f °C, Humidity: %.2f %%", temperature, humidity);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000)); // Read every 2 seconds
    }

    // Cleanup (unreachable in this example due to infinite loop)
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev_handle));
    ESP_ERROR_CHECK(i2c_del_master_bus(bus_handle));
    ESP_LOGI(TAG, "I2C de-initialized successfully");
}
