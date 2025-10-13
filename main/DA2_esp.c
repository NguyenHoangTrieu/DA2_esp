/*
 * HTU21D or Alternative Sensor Detection Example
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "i2c_sensor_detect";

// I2C Configuration
#define I2C_MASTER_SCL_IO           8
#define I2C_MASTER_SDA_IO           7
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TIMEOUT_MS       100

// Possible sensor addresses
#define HTU21D_ADDR                 0x40    // HTU21D address
#define MCP9808_ADDR                0x18    // MCP9808 temperature sensor
#define LIS3DH_ADDR                 0x18    // LIS3DH accelerometer (same as MCP9808)

// MCP9808 Registers
#define MCP9808_REG_CONFIG          0x01
#define MCP9808_REG_AMBIENT_TEMP    0x05
#define MCP9808_REG_MANUF_ID        0x06    // Should read 0x0054
#define MCP9808_REG_DEVICE_ID       0x07    // Should read 0x0400

// HTU21D Commands
#define HTU21D_TEMP_HOLD_CMD        0xE3
#define HTU21D_HUMID_HOLD_CMD       0xE5
#define HTU21D_SOFT_RESET           0xFE
#define HTU21D_READ_USER_REG        0xE7

/**
 * @brief Scan I2C bus for devices
 */
static void i2c_scanner(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "=== Scanning I2C bus ===");
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
        ESP_LOGI(TAG, "Total: %d device(s) found", devices_found);
    }
}

/**
 * @brief Read 16-bit register from MCP9808
 */
static esp_err_t mcp9808_read_reg16(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, 2, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return ret;
}

/**
 * @brief Identify sensor at address 0x18
 */
static void identify_sensor_0x18(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "\n=== Identifying sensor at 0x18 ===");
    
    // Try to add device at 0x18
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x18,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device at 0x18");
        return;
    }
    
    // Try reading MCP9808 manufacturer ID
    uint16_t manuf_id = 0;
    ret = mcp9808_read_reg16(dev_handle, MCP9808_REG_MANUF_ID, &manuf_id);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Read Manufacturer ID: 0x%04X", manuf_id);
        if (manuf_id == 0x0054) {
            ESP_LOGI(TAG, "*** Sensor identified as MCP9808 Temperature Sensor ***");
            
            // Read device ID
            uint16_t device_id = 0;
            mcp9808_read_reg16(dev_handle, MCP9808_REG_DEVICE_ID, &device_id);
            ESP_LOGI(TAG, "Device ID: 0x%04X (expected 0x0400)", device_id);
            
            // Read temperature
            uint16_t temp_raw = 0;
            if (mcp9808_read_reg16(dev_handle, MCP9808_REG_AMBIENT_TEMP, &temp_raw) == ESP_OK) {
                // Calculate temperature
                temp_raw &= 0x1FFF; // Clear flag bits
                float temperature = (temp_raw & 0x0FFF) / 16.0;
                if (temp_raw & 0x1000) {
                    temperature -= 256;
                }
                ESP_LOGI(TAG, "Temperature: %.2f °C", temperature);
            }
        } else {
            ESP_LOGW(TAG, "Unknown device with Manufacturer ID: 0x%04X", manuf_id);
        }
    } else {
        ESP_LOGE(TAG, "Failed to read from 0x18: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Sensor might be LIS3DH accelerometer (different read method needed)");
    }
    
    i2c_master_bus_rm_device(dev_handle);
}

/**
 * @brief Try to communicate with HTU21D at 0x40
 */
static void test_htu21d(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "\n=== Testing HTU21D at 0x40 ===");
    
    // Probe first
    esp_err_t ret = i2c_master_probe(bus_handle, HTU21D_ADDR, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTU21D NOT found at 0x40!");
        ESP_LOGE(TAG, "Check wiring:");
        ESP_LOGE(TAG, "  - HTU21D VIN to 3.3V");
        ESP_LOGE(TAG, "  - HTU21D GND to GND");
        ESP_LOGE(TAG, "  - HTU21D SDA to GPIO7");
        ESP_LOGE(TAG, "  - HTU21D SCL to GPIO8");
        ESP_LOGE(TAG, "  - Check if pull-up resistors are present (4.7kΩ)");
        return;
    }
    
    ESP_LOGI(TAG, "HTU21D found at 0x40!");
    
    // Add device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21D_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add HTU21D device");
        return;
    }
    
    // Try soft reset (optional, not critical)
    uint8_t cmd = HTU21D_SOFT_RESET;
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Soft reset sent successfully");
        vTaskDelay(pdMS_TO_TICKS(15));
    } else {
        ESP_LOGW(TAG, "Soft reset failed (not critical): %s", esp_err_to_name(ret));
    }
    
    // Try reading user register
    cmd = HTU21D_READ_USER_REG;
    uint8_t user_reg;
    ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, &user_reg, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "User Register: 0x%02X", user_reg);
        
        // Try reading temperature
        cmd = HTU21D_TEMP_HOLD_CMD;
        uint8_t temp_data[3];
        ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, temp_data, 3, I2C_MASTER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            uint16_t raw = ((uint16_t)temp_data[0] << 8) | temp_data[1];
            raw &= 0xFFFC;
            float temperature = -46.85 + 175.72 * ((float)raw / 65536.0);
            ESP_LOGI(TAG, "Temperature: %.2f °C", temperature);
        }
    } else {
        ESP_LOGE(TAG, "Failed to read user register: %s", esp_err_to_name(ret));
    }
    
    i2c_master_bus_rm_device(dev_handle);
}

/**
 * @brief Initialize I2C master bus
 */
static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C bus initialized (SDA=GPIO%d, SCL=GPIO%d)", 
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return ESP_OK;
}

void app_main(void)
{
    i2c_master_bus_handle_t bus_handle;
    
    // Initialize I2C
    ESP_ERROR_CHECK(i2c_master_init(&bus_handle));
    
    // Scan bus to find all devices
    i2c_scanner(bus_handle);
    
    // Identify sensor at 0x18
    identify_sensor_0x18(bus_handle);
    
    // Test HTU21D at 0x40
    test_htu21d(bus_handle);
    
    ESP_LOGI(TAG, "\n=== Test Complete ===");
    
    // Cleanup
    i2c_del_master_bus(bus_handle);
}
