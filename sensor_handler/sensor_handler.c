#include "sensor_handler.h"
#include "mqtt_handler.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"  // New ESP-IDF ADC API
#include <stdio.h>
#include <string.h>

#define TAG "sensor_handler"

// Pin config
#define SOIL_ADC_GPIO       32
#define SOIL_ADC_CHANNEL    ADC_CHANNEL_4    // GPIO32 => channel 4
#define SOIL_DIGITAL_GPIO   GPIO_NUM_26
#define RELAY_GPIO          GPIO_NUM_15

#define HTU21_I2C_PORT      I2C_NUM_0
#define HTU21_SDA           GPIO_NUM_21
#define HTU21_SCL           GPIO_NUM_22
#define HTU21_ADDR          0x40
#define HTU21_TEMP_CMD      0xE3
#define HTU21_HUMID_CMD     0xE5

#define SOIL_DRY_THRESHOLD  1800

static int16_t s_temp_x100 = 0;
static int16_t s_humid_x100 = 0;
static int s_soil_adc = 0;
static uint8_t s_soil_dig = 0;
static int s_soil_thres = SOIL_DRY_THRESHOLD;

// HTU21 read function (int, not float)
static esp_err_t htu21_read(int16_t *temp100, int16_t *humid100) {
    uint8_t data[3];
    uint16_t raw;
    esp_err_t ret;

    ret = i2c_master_write_read_device(
        HTU21_I2C_PORT, HTU21_ADDR,
        (uint8_t[]){HTU21_TEMP_CMD}, 1, data, 3, 100 / portTICK_PERIOD_MS);
    if (ret != ESP_OK) return ret;
    raw = ((uint16_t)data[0] << 8) | data[1]; raw &= 0xFFFC;
    int32_t temp = (-4685) + (17572L * raw) / 65536;
    *temp100 = (int16_t)temp;

    ret = i2c_master_write_read_device(
        HTU21_I2C_PORT, HTU21_ADDR,
        (uint8_t[]){HTU21_HUMID_CMD}, 1, data, 3, 100 / portTICK_PERIOD_MS);
    if (ret != ESP_OK) return ret;
    raw = ((uint16_t)data[0] << 8) | data[1]; raw &= 0xFFFC;
    int32_t humid = (-600) + (12500L * raw) / 65536;
    *humid100 = (int16_t)humid;

    return ESP_OK;
}

void sensor_set_soil_threshold(uint16_t thres) { s_soil_thres = thres; }
int16_t sensor_get_temp_x100(void) { return s_temp_x100; }
int16_t sensor_get_humid_x100(void) { return s_humid_x100; }
int sensor_get_soil_analog(void) { return s_soil_adc; }
uint8_t sensor_get_soil_digital(void) { return s_soil_dig; }

static void sensor_task(void *arg) {
    // I2C & GPIO config
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = HTU21_SDA,
        .scl_io_num = HTU21_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };
    i2c_param_config(HTU21_I2C_PORT, &i2c_conf);
    i2c_driver_install(HTU21_I2C_PORT, i2c_conf.mode, 0, 0, 0);

    gpio_set_direction(SOIL_DIGITAL_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    // ADC oneshot config
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1
    };
    adc_oneshot_new_unit(&init_cfg, &adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11,
    };
    adc_oneshot_config_channel(adc_handle, SOIL_ADC_CHANNEL, &chan_cfg);

    while (1) {
        // Read HTU21
        htu21_read(&s_temp_x100, &s_humid_x100);

        // Read ADC soil moisture
        int adc_val = 0;
        adc_oneshot_read(adc_handle, SOIL_ADC_CHANNEL, &adc_val);
        s_soil_adc = adc_val;

        // Read soil digital IO
        s_soil_dig = gpio_get_level(SOIL_DIGITAL_GPIO);

        // Pump relay logic
        gpio_set_level(RELAY_GPIO, (s_soil_adc < s_soil_thres) ? 1 : 0);

        // Telemetry formatting
        char telemetry[128];
        snprintf(telemetry, sizeof(telemetry),
            "{\"temp\":%d.%02d,\"humid\":%d.%02d,\"soil_adc\":%d,\"soil_dig\":%d,\"pump\":%d}",
            s_temp_x100 / 100, abs(s_temp_x100 % 100),
            s_humid_x100 / 100, abs(s_humid_x100 % 100),
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
