#include "sensor_handler.h"
#include "mqtt_handler.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "sensor_handler"

// I2C configuration for ESP32-P4 WiFi Dev Kit
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SDA_IO      7  // GPIO7 for SDA
#define I2C_MASTER_SCL_IO      8  // GPIO8 for SCL
#define I2C_MASTER_FREQ_HZ     100000  // 100kHz (safe for AHT20)
#define I2C_MASTER_TIMEOUT_MS  1000

// // --- Soil sensor pins ---
// #define SOIL_ADC_GPIO       32
// #define SOIL_ADC_CHANNEL    ADC_CHANNEL_4
// #define SOIL_DIGITAL_GPIO   GPIO_NUM_26
// #define RELAY_GPIO          GPIO_NUM_15
// #define SOIL_DRY_THRESHOLD  1800

// static uint16_t s_soil_adc = 0;
// static uint8_t s_soil_dig = 0;
// static int s_soil_thres = SOIL_DRY_THRESHOLD;
// adc_oneshot_unit_handle_t adc_handle;

// AHT20 (7bit) I2C address
#define AHT20_ADDR             0x38

// AHT20 commands 
#define AHT20_CMD_INIT         0xBE
#define AHT20_CMD_START_MEAS   0xAC
#define AHT20_CMD_SOFT_RESET   0xBA
#define AHT20_CMD_STATUS       0x71

#define AHT20_INIT_ARG1        0x08
#define AHT20_INIT_ARG2        0x00
#define AHT20_MEAS_ARG1        0x33
#define AHT20_MEAS_ARG2        0x00

#define AHT20_STATUS_BUSY_MASK 0x80
#define AHT20_STATUS_CAL_MASK  0x08

static int16_t s_temp_x100 = 0;
static int16_t s_humid_x100 = 0;

/**
 * @brief Initialize the AHT20 sensor as per datasheet.
 * @note If calibration is needed, sends the init command.
 */
static esp_err_t aht20_init(i2c_master_dev_handle_t dev_handle)
{
    vTaskDelay(pdMS_TO_TICKS(40)); // Wait sensor startup

    // Read status to check calibration
    uint8_t status_cmd = AHT20_CMD_STATUS;
    uint8_t status = 0;
    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &status_cmd, 1, &status, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read status register: %s", esp_err_to_name(ret));
        return ret;
    }

    // If not calibrated (CAL=0), send init command
    if ((status & AHT20_STATUS_CAL_MASK) == 0) {
        ESP_LOGI(TAG, "Sensor not calibrated. Sending init command...");
        uint8_t init_cmd[3] = {AHT20_CMD_INIT, AHT20_INIT_ARG1, AHT20_INIT_ARG2};
        ret = i2c_master_transmit(dev_handle, init_cmd, 3, I2C_MASTER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send init command: %s", esp_err_to_name(ret));
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // Wait for sensor to calibrate
    }
    ESP_LOGI(TAG, "AHT20 initialized OK");
    return ESP_OK;
}

/**
 * @brief Soft reset the AHT20 sensor.
 */
static esp_err_t aht20_soft_reset(i2c_master_dev_handle_t dev_handle)
{
    uint8_t cmd = AHT20_CMD_SOFT_RESET;
    esp_err_t ret = i2c_master_transmit(dev_handle, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(20)); // Allow sensor reset time
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send soft reset command: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AHT20 soft reset completed");
    return ESP_OK;
}

/**
 * @brief Trigger a measurement and read temperature/humidity from AHT20.
 * @param temp_x100 Output variable for temperature x100 (°C)
 * @param humid_x100 Output variable for humidity x100 (%RH)
 */
static esp_err_t aht20_read(i2c_master_dev_handle_t dev_handle, int16_t *temp_x100, int16_t *humid_x100)
{
    // 1. Trigger measurement
    uint8_t trig_cmd[3] = {AHT20_CMD_START_MEAS, AHT20_MEAS_ARG1, AHT20_MEAS_ARG2};
    esp_err_t ret = i2c_master_transmit(dev_handle, trig_cmd, 3, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send measurement command: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Wait for measurement (datasheet: min ~80ms)
    vTaskDelay(pdMS_TO_TICKS(90));

    // 3. Read status to ensure not busy (optional)
    uint8_t status_cmd = AHT20_CMD_STATUS;
    uint8_t status = 0;
    int try_count = 0;
    do {
        ret = i2c_master_transmit_receive(dev_handle, &status_cmd, 1, &status, 1, I2C_MASTER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read status: %s", esp_err_to_name(ret));
            return ret;
        }
        if (!(status & AHT20_STATUS_BUSY_MASK)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
        try_count++;
    } while ((status & AHT20_STATUS_BUSY_MASK) && try_count < 10);

    if (status & AHT20_STATUS_BUSY_MASK) {
        ESP_LOGE(TAG, "AHT20 is still busy after waiting!");
        return ESP_ERR_TIMEOUT;
    }

    // 4. Read 6 bytes (status + 5 data)
    uint8_t rx_data[6];
    ret = i2c_master_receive(dev_handle, rx_data, 6, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read measurement data: %s", esp_err_to_name(ret));
        return ret;
    }

    // 5. Parse 20 bits for humidity and temp (see datasheet)
    uint32_t hum_raw  = ((uint32_t)rx_data[1] << 12) | ((uint32_t)rx_data[2] << 4) | ((uint32_t)(rx_data[3] & 0xF0) >> 4);
    uint32_t temp_raw = (((uint32_t)(rx_data[3] & 0x0F)) << 16) | ((uint32_t)rx_data[4] << 8) | rx_data[5];

    // Convert to float for calculation
    float hum  = ((float)hum_raw)  / 1048576.0f * 100.0f; // %RH
    float temp = ((float)temp_raw) / 1048576.0f * 200.0f - 50.0f; // degree C

    // Save as int x100
    *humid_x100 = (int16_t)(hum * 100.0f);
    *temp_x100  = (int16_t)(temp * 100.0f);

    return ESP_OK;
}

/**
 * @brief Initialize I2C bus and AHT20 device.
 */
static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle)
{
    // Configure the I2C master bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, // ESP32-P4-DevKit has external pull-ups
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized on SDA=%d, SCL=%d", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    // Configure the AHT20 device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle));
    ESP_LOGI(TAG, "AHT20 device added to I2C bus (address: 0x%02X)", AHT20_ADDR);

    return ESP_OK;
}

// void soil_sensor_init(adc_oneshot_unit_handle_t *adc_handle) {
//     adc_oneshot_unit_init_cfg_t adc_cfg = {
//         .unit_id = ADC_UNIT_1,
//     };
//     ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, adc_handle));

//     adc_oneshot_chan_cfg_t chan_cfg = {
//         .bitwidth = ADC_BITWIDTH_12,
//         .atten = ADC_ATTEN_DB_12,
//     };
//     ESP_ERROR_CHECK(adc_oneshot_config_channel(*adc_handle, SOIL_ADC_CHANNEL, &chan_cfg));

//     // Configure digital pin
//     gpio_set_direction(SOIL_DIGITAL_GPIO, GPIO_MODE_INPUT);
//     // Configure relay pin
//     gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
// }

/**
 * @brief Task to periodically read temperature/humidity from AHT20 and publish via MQTT.
 */
static void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;

    // Initialize I2C and sensor
    if (i2c_master_init(&bus_handle, &dev_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        vTaskDelete(NULL);
        return;
    }

    // Optional: Soft-reset sensor at startup
    aht20_soft_reset(dev_handle);

    // Always attempt to initialize the sensor (calibration check)
    if (aht20_init(dev_handle) != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init failed!");
        vTaskDelete(NULL);
        return;
    }

    // // --- ADC setup for analog soil sensor ---
    // soil_sensor_init(&adc_handle);

    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        // Read soil moisture analog value
        int adc_val = 0;
        adc_oneshot_read(adc_handle, SOIL_ADC_CHANNEL, &adc_val);
        s_soil_adc = (uint16_t)adc_val;

        // Read digital soil sensor
        s_soil_dig = gpio_get_level(SOIL_DIGITAL_GPIO);

        // Control relay for pump (on if soil is dry)
        gpio_set_level(RELAY_GPIO, (s_soil_adc < s_soil_thres) ? 1 : 0);

        if (aht20_read(dev_handle, &s_temp_x100, &s_humid_x100) == ESP_OK) {
          ESP_LOGI(TAG, "Temperature: %d.%02d °C, Humidity: %d.%02d %%",
                   s_temp_x100 / 100, abs(s_temp_x100 % 100),
                   s_humid_x100 / 100, abs(s_humid_x100 % 100));
        } else {
          ESP_LOGE(TAG, "Failed to read AHT20 sensor");
        }

    //    // Build telemetry data (JSON)
    //     char telemetry[128];
    //     snprintf(telemetry, sizeof(telemetry),
    //         "{\"temp\":%d.%02d,\"humid\":%d.%02d,\"soil_adc\":%u,\"soil_dig\":%u}",
    //         s_temp_x100/100, abs(s_temp_x100%100),
    //         s_humid_x100/100, abs(s_humid_x100%100),
    //         s_soil_adc, s_soil_dig
    //     );
    //     ESP_LOGI(TAG, "Telemetry: %s", telemetry);
    //     mqtt_build_telemetry_payload(telemetry, strlen(telemetry));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Start the sensor handler (task creation stub).
 */
void sensor_handler_start(void)
{
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Sensor handler started");
}

/**
 * @brief Get current temperature value (x100)
 */
int16_t sensor_get_temp_x100(void)
{
    return s_temp_x100;
}

/**
 * @brief Get current humidity value (x100)
 */
int16_t sensor_get_humid_x100(void)
{
    return s_humid_x100;
}
