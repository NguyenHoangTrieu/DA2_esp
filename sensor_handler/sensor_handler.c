#include "sensor_handler.h"
#include "mqtt_handler.h"  // For mqtt_build_telemetry_payload()
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TAG "sensor_handler"

// GPIO and ADC configuration (adjust as needed for your board)
#define SOIL_ADC_CHANNEL    ADC1_CHANNEL_4    // Example channel for analog soil sensor (GPIO32)
#define SOIL_DIGITAL_GPIO   GPIO_NUM_26       // Digital output from soil sensor (active high/low)
#define RELAY_GPIO          GPIO_NUM_15       // GPIO controlling pump relay

#define HTU21_I2C_PORT      I2C_NUM_0
#define HTU21_SDA           GPIO_NUM_21
#define HTU21_SCL           GPIO_NUM_22

// Default soil moisture threshold: below this value = soil is dry, turn on pump
#define SOIL_DRY_THRESHOLD  1800    

// Internal variables to store sensor readings
static float s_temp = 0;
static float s_humid = 0;
static uint16_t s_soil_adc = 0;
static uint8_t s_soil_dig = 0;
static uint16_t s_soil_thres = SOIL_DRY_THRESHOLD;

/**
 * Read HTU21 temperature and humidity sensor.
 * Replace this with your actual I2C driver for HTU21.
 * Returns ESP_OK on success, error code otherwise.
 */
static esp_err_t htu21_read(float *temp, float *humid) {
    // Placeholder: Replace with real I2C read code!
    *temp = 27.7f;      
    *humid = 60.0f;
    return ESP_OK;
}

// Set custom threshold for determining when soil is "dry"
void sensor_set_soil_threshold(uint16_t thres) { s_soil_thres = thres; }

// Getters for current sensor readings
float sensor_get_temp(void) { return s_temp; }
float sensor_get_humid(void) { return s_humid; }
uint16_t sensor_get_soil_analog(void) { return s_soil_adc; }
uint8_t sensor_get_soil_digital(void) { return s_soil_dig; }

/**
 * Main FreeRTOS task for sensor acquisition and relay control.
 * Reads all sensors every 1000ms, updates relay, and builds telemetry for MQTT.
 */
static void sensor_task(void *arg) {
    // Configure ADC for 12-bit width
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SOIL_ADC_CHANNEL, ADC_ATTEN_DB_11);
    gpio_set_direction(SOIL_DIGITAL_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        // 1. Read temperature and humidity (HTU21)
        htu21_read(&s_temp, &s_humid);

        // 2. Read analog soil moisture sensor (ADC)
        s_soil_adc = adc1_get_raw(SOIL_ADC_CHANNEL);

        // 3. Read digital soil moisture GPIO
        s_soil_dig = gpio_get_level(SOIL_DIGITAL_GPIO);

        // 4. Activate relay if soil is dry (analog value below threshold)
        if (s_soil_adc < s_soil_thres) {
            gpio_set_level(RELAY_GPIO, 1); // Turn ON pump
        } else {
            gpio_set_level(RELAY_GPIO, 0); // Turn OFF pump
        }

        // 5. Build telemetry payload and queue to MQTT for publishing
        char telemetry[256];
        snprintf(telemetry, sizeof(telemetry),
            "{\"temp\":%.2f, \"humid\":%.2f, \"soil_adc\":%u, \"soil_dig\":%u, \"pump\":%d}",
            s_temp, s_humid, s_soil_adc, s_soil_dig, gpio_get_level(RELAY_GPIO)
        );
        ESP_LOGI(TAG, "Telemetry: %s", telemetry);
        mqtt_build_telemetry_payload(telemetry, strlen(telemetry));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * Start the sensor handler task.
 * Call this function once in app_main().
 */
void sensor_handler_start(void) {
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 6, NULL);
}
