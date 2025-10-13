/*
 * Simple HTU21D Test for ESP32-P4-WIFI6-DEV-KIT
 * Make sure HTU21D sensor is connected before running!
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "htu21d_test";

// I2C Configuration
#define I2C_MASTER_SCL_IO           8
#define I2C_MASTER_SDA_IO           7
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TIMEOUT_MS       2000

// HTU21D
#define HTU21D_ADDR                 0x40
#define HTU21D_TEMP_HOLD_CMD        0xE3
#define HTU21D_HUMID_HOLD_CMD       0xE5
#define HTU21D_SOFT_RESET           0xFE

void app_main(void)
{
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "HTU21D Sensor Test");
    ESP_LOGI(TAG, "Board: ESP32-P4-WIFI6-DEV-KIT");
    ESP_LOGI(TAG, "========================================\n");
    
    ESP_LOGW(TAG, "IMPORTANT: Make sure HTU21D is connected:");
    ESP_LOGW(TAG, "  HTU21D VCC  -> 3.3V");
    ESP_LOGW(TAG, "  HTU21D GND  -> GND");
    ESP_LOGW(TAG, "  HTU21D SDA  -> GPIO7");
    ESP_LOGW(TAG, "  HTU21D SCL  -> GPIO8\n");
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Initialize I2C bus
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // Board has external pull-ups
    };
    
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized (GPIO7=SDA, GPIO8=SCL, 100kHz)");
    
    // Probe HTU21D
    ESP_LOGI(TAG, "Searching for HTU21D sensor at address 0x40...");
    esp_err_t ret = i2c_master_probe(bus_handle, HTU21D_ADDR, I2C_MASTER_TIMEOUT_MS);
    
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "\n*** I2C TIMEOUT ***");
        ESP_LOGE(TAG, "No device responded on the I2C bus.");
        ESP_LOGE(TAG, "\nTroubleshooting:");
        ESP_LOGE(TAG, "1. Is the HTU21D sensor connected?");
        ESP_LOGE(TAG, "2. Check wiring (VCC, GND, SDA, SCL)");
        ESP_LOGE(TAG, "3. Measure voltage on VCC pin (should be 3.3V)");
        ESP_LOGE(TAG, "4. Check if sensor module has pull-up resistors");
        ESP_LOGE(TAG, "5. Try another sensor module (might be faulty)");
        ESP_LOGE(TAG, "\nIf you don't have an HTU21D sensor, you cannot");
        ESP_LOGE(TAG, "run this test. Please connect the sensor first.\n");
        i2c_del_master_bus(bus_handle);
        return;
    }
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "\n*** HTU21D NOT FOUND ***");
        ESP_LOGE(TAG, "I2C bus is working, but no device at address 0x40");
        ESP_LOGE(TAG, "Make sure you have an HTU21D sensor (not another sensor)\n");
        i2c_del_master_bus(bus_handle);
        return;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Probe error: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        return;
    }
    
    ESP_LOGI(TAG, "*** HTU21D FOUND! ***\n");
    
    // Add device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21D_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));
    
    // Soft reset
    uint8_t cmd = HTU21D_SOFT_RESET;
    ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Soft reset sent");
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        ESP_LOGW(TAG, "Soft reset failed (continuing anyway)");
    }
    
    // Read loop
    ESP_LOGI(TAG, "Starting continuous reading...\n");
    
    while (1) {
        // Read temperature
        cmd = HTU21D_TEMP_HOLD_CMD;
        uint8_t temp_data[3];
        ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, temp_data, 3, I2C_MASTER_TIMEOUT_MS);
        
        if (ret == ESP_OK) {
            uint16_t raw = ((uint16_t)temp_data[0] << 8) | temp_data[1];
            raw &= 0xFFFC;
            float temperature = -46.85 + 175.72 * ((float)raw / 65536.0);
            
            // Read humidity
            cmd = HTU21D_HUMID_HOLD_CMD;
            uint8_t humid_data[3];
            ret = i2c_master_transmit_receive(dev_handle, &cmd, 1, humid_data, 3, I2C_MASTER_TIMEOUT_MS);
            
            if (ret == ESP_OK) {
                raw = ((uint16_t)humid_data[0] << 8) | humid_data[1];
                raw &= 0xFFFC;
                float humidity = -6.0 + 125.0 * ((float)raw / 65536.0);
                if (humidity < 0) humidity = 0;
                if (humidity > 100) humidity = 100;
                
                ESP_LOGI(TAG, "Temperature: %.2f °C  |  Humidity: %.2f %%", temperature, humidity);
            }
        } else {
            ESP_LOGE(TAG, "Read error: %s", esp_err_to_name(ret));
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    // Cleanup
    i2c_master_bus_rm_device(dev_handle);
    i2c_del_master_bus(bus_handle);
}
