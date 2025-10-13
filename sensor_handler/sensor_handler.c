#include "sensor_handler.h"
#include "mqtt_handler.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TAG "sensor_handler"

// Pin config
#define SOIL_ADC_GPIO       32
#define SOIL_ADC_CHANNEL    ADC_CHANNEL_4
#define SOIL_DIGITAL_GPIO   GPIO_NUM_26
#define RELAY_GPIO          GPIO_NUM_15

#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_FREQ_HZ  100000

#define HTU21_ADDR          0x40
#define HTU21_TEMP_CMD      0xE3
#define HTU21_HUMID_CMD     0xE5
#define SOIL_DRY_THRESHOLD  1800

static int16_t s_temp_x100 = 0;
static int16_t s_humid_x100 = 0;
static int s_soil_adc = 0;
static uint8_t s_soil_dig = 0;
static int s_soil_thres = SOIL_DRY_THRESHOLD;

// HTU21 sensor I2C read using driver/i2c_master.h
static esp_err_t htu21_read(int16_t *temp100, int16_t *humid100, i2c_master_dev_handle_t dev_handle) {
    uint8_t data[3];
    uint16_t raw;
    esp_err_t ret;

    // Read Temperature
    ret = i2c_master_transmit(dev_handle, &HTU21_TEMP_CMD, 1, 1000);
    if (ret != ESP_OK) return ret;
    ret = i2c_master_receive(dev_handle, data, 3, 1000);
    if (ret != ESP_OK) return ret;
    raw = ((uint16_t)data[0] << 8) | data[1]; raw &= 0xFFFC;
    int32_t temp = -4685 + (17572L * raw) / 65536;
    *temp100 = temp;

    // Read Humidity
    ret = i2c_master_transmit(dev_handle, &HTU21_HUMID_CMD, 1, 1000);
    if (ret != ESP_OK) return ret;
    ret = i2c_master_receive(dev_handle, data, 3, 1000);
    if (ret != ESP_OK) return ret;
    raw = ((uint16_t)data[0] << 8) | data[1]; raw &= 0xFFFC;
    int32_t humid = -600 + (12500L * raw) / 65536;
    *humid100 = humid;

    return ESP_OK;
}

void sensor_set_soil_threshold(uint16_t thres) { s_soil_thres = thres; }
int16_t sensor_get_temp_x100(void) { return s_temp_x100; }
int16_t sensor_get_humid_x100(void) { return s_humid_x100; }
int sensor_get_soil_analog(void) { return s_soil_adc; }
uint8_t sensor_get_soil_digital(void) { return s_soil_dig; }

static void sensor_task(void *arg) {
    // ADC setup (oneshot)
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &adc_handle);
    adc_oneshot_chan_cfg_t chan_cfg = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_11 };
    adc_oneshot_config_channel(adc_handle, SOIL_ADC_CHANNEL, &chan_cfg);

    gpio_set_direction(SOIL_DIGITAL_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    // I2C setup using i2c_master API
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    while (1) {
        htu21_read(&s_temp_x100, &s_humid_x100, dev_handle);
        adc_oneshot_read(adc_handle, SOIL_ADC_CHANNEL, &s_soil_adc);
        s_soil_dig = gpio_get_level(SOIL_DIGITAL_GPIO);

        gpio_set_level(RELAY_GPIO, (s_soil_adc < s_soil_thres) ? 1 : 0);

        char telemetry[128];
        snprintf(telemetry, sizeof(telemetry),
            "{\"temp\":%d.%02d,\"humid\":%d.%02d,\"soil_adc\":%d,\"soil_dig\":%d,\"pump\":%d}",
            s_temp_x100/100, abs(s_temp_x100%100),
            s_humid_x100/100, abs(s_humid_x100%100),
            s_soil_adc, s_soil_dig, gpio_get_level(RELAY_GPIO)
        );
        mqtt_build_telemetry_payload(telemetry, strlen(telemetry));
        ESP_LOGI(TAG, "Telemetry: %s", telemetry);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void sensor_handler_start(void) {
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
