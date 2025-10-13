/*
 * I2C Diagnostic Tool for ESP32-P4-WIFI6-DEV-KIT
 * Tests I2C bus and detects connected sensors
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "i2c_diagnostic";

// I2C Configuration for ESP32-P4-WIFI6-DEV-KIT
#define I2C_MASTER_SCL_IO           8               // GPIO8
#define I2C_MASTER_SDA_IO           7               // GPIO7
#define I2C_MASTER_NUM              I2C_NUM_0       
#define I2C_MASTER_FREQ_HZ          100000          // 100kHz (safe speed)
#define I2C_MASTER_TIMEOUT_MS       2000            // Increased timeout

// Known sensor addresses
#define HTU21D_ADDR                 0x40
#define MCP9808_ADDR                0x18
#define BMP280_ADDR                 0x76
#define BME280_ADDR_1               0x76
#define BME280_ADDR_2               0x77

/**
 * @brief Test GPIO pins directly
 */
static void test_gpio_pins(void)
{
    ESP_LOGI(TAG, "\n=== Testing GPIO Pins ===");
    
    // Configure GPIO7 (SDA) and GPIO8 (SCL) as input with pull-up
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Read GPIO levels
    int sda_level = gpio_get_level(I2C_MASTER_SDA_IO);
    int scl_level = gpio_get_level(I2C_MASTER_SCL_IO);
    
    ESP_LOGI(TAG, "GPIO7 (SDA) level: %d (should be 1 if pull-up works)", sda_level);
    ESP_LOGI(TAG, "GPIO8 (SCL) level: %d (should be 1 if pull-up works)", scl_level);
    
    if (sda_level == 0 || scl_level == 0) {
        ESP_LOGE(TAG, "WARNING: One or both I2C lines are LOW!");
        ESP_LOGE(TAG, "Possible causes:");
        ESP_LOGE(TAG, "  - No pull-up resistors on the bus");
        ESP_LOGE(TAG, "  - A device is holding the line LOW (bus stuck)");
        ESP_LOGE(TAG, "  - Short circuit to ground");
    } else {
        ESP_LOGI(TAG, "GPIO pins look OK - both lines are HIGH");
    }
    
    // Reset GPIO to default state
    gpio_reset_pin(I2C_MASTER_SDA_IO);
    gpio_reset_pin(I2C_MASTER_SCL_IO);
}

/**
 * @brief Scan I2C bus with extended timeout
 */
static void i2c_scanner_detailed(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "\n=== Scanning I2C Bus (0x01-0x7F) ===");
    ESP_LOGI(TAG, "This may take a while...");
    
    int devices_found = 0;
    
    for (uint8_t addr = 1; addr < 127; addr++) {
        // Use longer timeout for scanning
        esp_err_t ret = i2c_master_probe(bus_handle, addr, 2000);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Device FOUND at address 0x%02X", addr);
            devices_found++;
            
            // Identify common devices
            switch (addr) {
                case 0x18:
                    ESP_LOGI(TAG, "  -> Possible device: MCP9808 or LIS3DH");
                    break;
                case 0x40:
                    ESP_LOGI(TAG, "  -> Possible device: HTU21D");
                    break;
                case 0x76:
                case 0x77:
                    ESP_LOGI(TAG, "  -> Possible device: BMP280/BME280");
                    break;
                default:
                    break;
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timeout at address 0x%02X - stopping scan", addr);
            ESP_LOGE(TAG, "This usually means I2C bus is not working properly");
            break;
        }
        // Else: ESP_ERR_NOT_FOUND is expected for unused addresses
    }
    
    ESP_LOGI(TAG, "\n=== Scan Complete ===");
    if (devices_found == 0) {
        ESP_LOGE(TAG, "NO I2C devices found!");
        ESP_LOGE(TAG, "\nPossible issues:");
        ESP_LOGE(TAG, "1. No sensor connected to the I2C bus");
        ESP_LOGE(TAG, "2. Incorrect wiring:");
        ESP_LOGE(TAG, "   - Sensor VCC should connect to 3.3V");
        ESP_LOGE(TAG, "   - Sensor GND should connect to GND");
        ESP_LOGE(TAG, "   - Sensor SDA should connect to GPIO7");
        ESP_LOGE(TAG, "   - Sensor SCL should connect to GPIO8");
        ESP_LOGE(TAG, "3. Sensor not powered on");
        ESP_LOGE(TAG, "4. Faulty sensor module");
        ESP_LOGE(TAG, "5. Wrong I2C pins (check board documentation)");
    } else {
        ESP_LOGI(TAG, "Found %d device(s)", devices_found);
    }
}

/**
 * @brief Initialize I2C master bus - CORRECTED for ESP32-P4-WIFI6-DEV-KIT
 */
static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle)
{
    ESP_LOGI(TAG, "\n=== Initializing I2C Bus ===");
    
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // IMPORTANT: Board has external pull-ups!
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C Configuration:");
    ESP_LOGI(TAG, "  Port: I2C_NUM_%d", I2C_MASTER_NUM);
    ESP_LOGI(TAG, "  SDA: GPIO%d", I2C_MASTER_SDA_IO);
    ESP_LOGI(TAG, "  SCL: GPIO%d", I2C_MASTER_SCL_IO);
    ESP_LOGI(TAG, "  Frequency: %d Hz", I2C_MASTER_FREQ_HZ);
    ESP_LOGI(TAG, "  Internal Pull-ups: DISABLED (board has external pull-ups)");
    ESP_LOGI(TAG, "I2C bus initialized successfully");
    
    return ESP_OK;
}

/**
 * @brief Test if we can create and communicate with a dummy device
 */
static void test_i2c_communication(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "\n=== Testing I2C Communication ===");
    
    // Try to probe a few common addresses with detailed error info
    uint8_t test_addresses[] = {0x18, 0x40, 0x76, 0x77};
    
    for (int i = 0; i < sizeof(test_addresses); i++) {
        uint8_t addr = test_addresses[i];
        ESP_LOGI(TAG, "Probing address 0x%02X...", addr);
        
        esp_err_t ret = i2c_master_probe(bus_handle, addr, 2000);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  SUCCESS - Device responded!");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "  No device (NACK received - this is normal)");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "  TIMEOUT - I2C bus not responding!");
            ESP_LOGE(TAG, "  This indicates a hardware problem:");
            ESP_LOGE(TAG, "    - SCL or SDA lines stuck");
            ESP_LOGE(TAG, "    - No pull-up resistors");
            ESP_LOGE(TAG, "    - Incorrect GPIO pins");
            break;
        } else {
            ESP_LOGE(TAG, "  Error: %s", esp_err_to_name(ret));
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    i2c_master_bus_handle_t bus_handle;
    
    ESP_LOGI(TAG, "\n");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-P4 I2C Diagnostic Tool");
    ESP_LOGI(TAG, "  Board: ESP32-P4-WIFI6-DEV-KIT");
    ESP_LOGI(TAG, "========================================");
    
    // Step 1: Test GPIO pins before I2C init
    test_gpio_pins();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Step 2: Initialize I2C bus
    esp_err_t ret = i2c_master_init(&bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C - cannot continue");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Step 3: Test basic I2C communication
    test_i2c_communication(bus_handle);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Step 4: Full bus scan
    i2c_scanner_detailed(bus_handle);
    
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  Diagnostic Complete");
    ESP_LOGI(TAG, "========================================");
    
    // Instructions
    ESP_LOGI(TAG, "\nNext steps:");
    ESP_LOGI(TAG, "1. If no devices found, check your wiring");
    ESP_LOGI(TAG, "2. If you get timeouts, check pull-up resistors");
    ESP_LOGI(TAG, "3. If device found, use the appropriate driver code");
    
    // Cleanup
    i2c_del_master_bus(bus_handle);
}
