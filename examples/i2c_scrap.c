/*
 * Automatic I2C Scanner for ESP32-P4-WIFI6-DEV-KIT
 * Automatically scans I2C bus on startup without requiring console input
 * 
 * Based on ESP-IDF i2c_tools example
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "i2c_auto_scanner";

// I2C Configuration for ESP32-P4-WIFI6-DEV-KIT
#define I2C_MASTER_SCL_IO           8       // GPIO8 for SCL
#define I2C_MASTER_SDA_IO           7       // GPIO7 for SDA (change to 9 if needed)
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000  // 100kHz
#define I2C_MASTER_TIMEOUT_MS       1000

// Known I2C device database for identification
typedef struct {
    uint8_t addr;
    const char *name;
} i2c_device_info_t;

static const i2c_device_info_t known_devices[] = {
    {0x18, "MCP9808 (Temp) / LIS3DH (Accel)"},
    {0x19, "LIS3DH (Accelerometer)"},
    {0x23, "BH1750 (Light Sensor)"},
    {0x38, "FT6236 (Touch Controller)"},
    {0x40, "HTU21D (Temp/Humidity) / PCA9685"},
    {0x44, "SHT31 (Temp/Humidity)"},
    {0x48, "ADS1115 (ADC) / TMP102 (Temp)"},
    {0x4C, "TMP102 (Temperature)"},
    {0x50, "EEPROM (24C series)"},
    {0x57, "EEPROM (24C series)"},
    {0x68, "MPU6050/MPU9250 (IMU) / DS1307 (RTC)"},
    {0x69, "MPU6050/MPU9250 (IMU alt)"},
    {0x76, "BMP280/BME280 (Pressure/Temp)"},
    {0x77, "BMP280/BME280 (Pressure/Temp alt)"},
};

static const int known_devices_count = sizeof(known_devices) / sizeof(i2c_device_info_t);

/**
 * @brief Look up device name from address
 */
static const char* get_device_name(uint8_t addr)
{
    for (int i = 0; i < known_devices_count; i++) {
        if (known_devices[i].addr == addr) {
            return known_devices[i].name;
        }
    }
    return "Unknown device";
}

/**
 * @brief Initialize I2C master bus
 */
static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // Board has external pull-ups
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C Configuration:");
    ESP_LOGI(TAG, "  Port: I2C_NUM_%d", I2C_MASTER_NUM);
    ESP_LOGI(TAG, "  SCL:  GPIO%d", I2C_MASTER_SCL_IO);
    ESP_LOGI(TAG, "  SDA:  GPIO%d", I2C_MASTER_SDA_IO);
    ESP_LOGI(TAG, "  Freq: %d Hz", I2C_MASTER_FREQ_HZ);
    ESP_LOGI(TAG, "  Internal pull-ups: Disabled (using external)");
    
    return ESP_OK;
}

/**
 * @brief Scan I2C bus for devices (similar to i2cdetect command)
 */
static void i2c_scan_bus(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "Starting I2C Bus Scan...");
    ESP_LOGI(TAG, "========================================\n");
    
    int devices_found = 0;
    uint8_t found_addresses[128];
    
    // Print header (like i2cdetect)
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            uint8_t address = i + j;
            
            esp_err_t ret = i2c_master_probe(bus_handle, address, I2C_MASTER_TIMEOUT_MS);
            
            if (ret == ESP_OK) {
                printf("%02x ", address);
                found_addresses[devices_found++] = address;
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\n");
    }
    
    printf("\n");
    
    // Print detailed device information
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Scan Results");
    ESP_LOGI(TAG, "========================================");
    
    if (devices_found == 0) {
        ESP_LOGE(TAG, "No I2C devices found!");
        ESP_LOGE(TAG, "\nPossible reasons:");
        ESP_LOGE(TAG, "  1. No sensor connected");
        ESP_LOGE(TAG, "  2. Incorrect wiring");
        ESP_LOGE(TAG, "  3. Sensor not powered");
        ESP_LOGE(TAG, "  4. Wrong GPIO pins");
        ESP_LOGE(TAG, "  5. Missing pull-up resistors");
    } else {
        ESP_LOGI(TAG, "Found %d device(s):\n", devices_found);
        
        for (int i = 0; i < devices_found; i++) {
            uint8_t addr = found_addresses[i];
            const char *name = get_device_name(addr);
            ESP_LOGI(TAG, "  [%d] Address: 0x%02X - %s", i + 1, addr, name);
        }
    }
    
    ESP_LOGI(TAG, "\n========================================\n");
}

/**
 * @brief Read a register from a device (similar to i2cget)
 */
static esp_err_t i2c_read_register(i2c_master_bus_handle_t bus_handle, 
                                    uint8_t device_addr, 
                                    uint8_t reg_addr, 
                                    uint8_t *data, 
                                    size_t len)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    
    i2c_master_bus_rm_device(dev_handle);
    
    return ret;
}

/**
 * @brief Dump all registers from a device (similar to i2cdump)
 */
static void i2c_dump_registers(i2c_master_bus_handle_t bus_handle, uint8_t device_addr)
{
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "Register Dump for Device 0x%02X", device_addr);
    ESP_LOGI(TAG, "========================================\n");
    
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device: %s", esp_err_to_name(ret));
        return;
    }
    
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f    0123456789abcdef\n");
    
    for (int i = 0; i < 256; i += 16) {
        printf("%02x: ", i);
        uint8_t block[16];
        
        for (int j = 0; j < 16; j++) {
            uint8_t reg_addr = i + j;
            uint8_t data;
            
            esp_err_t read_ret = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, &data, 1, I2C_MASTER_TIMEOUT_MS);
            
            if (read_ret == ESP_OK) {
                printf("%02x ", data);
                block[j] = data;
            } else {
                printf("XX ");
                block[j] = 0xFF;
            }
        }
        
        printf("   ");
        
        // Print ASCII representation
        for (int j = 0; j < 16; j++) {
            if (block[j] == 0xFF) {
                printf("X");
            } else if (block[j] == 0x00 || block[j] == 0xFF) {
                printf(".");
            } else if (block[j] < 32 || block[j] >= 127) {
                printf("?");
            } else {
                printf("%c", block[j]);
            }
        }
        printf("\n");
    }
    
    printf("\n");
    
    i2c_master_bus_rm_device(dev_handle);
}

/**
 * @brief Main scanning task
 */
static void i2c_scanner_task(void *arg)
{
    i2c_master_bus_handle_t bus_handle;
    
    // Wait a bit for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "\n");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Automatic I2C Scanner");
    ESP_LOGI(TAG, "  ESP32-P4-WIFI6-DEV-KIT");
    ESP_LOGI(TAG, "========================================\n");
    
    // Initialize I2C
    if (i2c_master_init(&bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus");
        vTaskDelete(NULL);
        return;
    }
    
    // Perform initial scan
    i2c_scan_bus(bus_handle);
    
    // Example: If device at 0x40 (HTU21D) is found, dump its registers
    uint8_t test_addr = 0x40;
    esp_err_t probe_ret = i2c_master_probe(bus_handle, test_addr, I2C_MASTER_TIMEOUT_MS);
    if (probe_ret == ESP_OK) {
        ESP_LOGI(TAG, "Device at 0x%02X detected, attempting register dump...\n", test_addr);
        vTaskDelay(pdMS_TO_TICKS(500));
        i2c_dump_registers(bus_handle, test_addr);
    }
    
    // Periodic scanning every 10 seconds
    ESP_LOGI(TAG, "Starting periodic scan (every 10 seconds)...\n");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "\n--- Periodic Scan ---");
        i2c_scan_bus(bus_handle);
    }
    
    // Cleanup (unreachable)
    i2c_del_master_bus(bus_handle);
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Create scanner task
    xTaskCreate(i2c_scanner_task, "i2c_scanner", 4096, NULL, 5, NULL);
}
