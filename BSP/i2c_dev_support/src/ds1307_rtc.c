/**
 * @file ds1307_rtc.c
 * @brief DS1307 RTC Driver Implementation
 */

#include "ds1307_rtc.h"
#include "i2c_dev_support.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DS1307_RTC";
static i2c_master_dev_handle_t ds1307_handle = NULL;

// DS1307 Register addresses
#define DS1307_REG_SECONDS  0x00
#define DS1307_REG_MINUTES  0x01
#define DS1307_REG_HOURS    0x02
#define DS1307_REG_DAY      0x03
#define DS1307_REG_DATE     0x04
#define DS1307_REG_MONTH    0x05
#define DS1307_REG_YEAR     0x06
#define DS1307_REG_CONTROL  0x07

// Helper: BCD to Decimal
static uint8_t bcd_to_dec(uint8_t val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

// Helper: Decimal to BCD
static uint8_t dec_to_bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

esp_err_t ds1307_init(void) {
    // Check if I2C support is initialized
    if (!i2c_dev_support_is_initialized()) {
        ESP_LOGE(TAG, "I2C not initialized. Call i2c_dev_support_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing DS1307 RTC at address 0x%02X", DS1307_I2C_ADDR);

    // Add DS1307 device to bus
    esp_err_t ret = i2c_dev_support_add_device(DS1307_I2C_ADDR, DS1307_I2C_FREQ_HZ, &ds1307_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add DS1307 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify DS1307 is present by reading control register
    uint8_t reg_addr = DS1307_REG_CONTROL;
    uint8_t control_val;
    ret = i2c_dev_support_write_read(ds1307_handle, &reg_addr, 1, &control_val, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS1307 not found at address 0x%02X", DS1307_I2C_ADDR);
        i2c_dev_support_remove_device(ds1307_handle);
        ds1307_handle = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // Check if clock is running
    bool is_running;
    ret = ds1307_is_running(&is_running);
    if (ret == ESP_OK && !is_running) {
        ESP_LOGW(TAG, "DS1307 clock halted, starting...");
        ds1307_start();
    }

    ESP_LOGI(TAG, "DS1307 RTC initialized successfully");
    return ESP_OK;
}

esp_err_t ds1307_deinit(void) {
    if (ds1307_handle) {
        esp_err_t ret = i2c_dev_support_remove_device(ds1307_handle);
        ds1307_handle = NULL;
        ESP_LOGI(TAG, "DS1307 RTC deinitialized");
        return ret;
    }
    return ESP_OK;
}

esp_err_t ds1307_read_time(struct tm *timeinfo) {
    if (!ds1307_handle || !timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = DS1307_REG_SECONDS;
    uint8_t data[7];

    esp_err_t ret = i2c_dev_support_write_read(ds1307_handle, &reg_addr, 1, data, 7, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from DS1307: %s", esp_err_to_name(ret));
        return ret;
    }

    // Parse BCD data
    timeinfo->tm_sec = bcd_to_dec(data[0] & 0x7F);
    timeinfo->tm_min = bcd_to_dec(data[1] & 0x7F);
    timeinfo->tm_hour = bcd_to_dec(data[2] & 0x3F);
    timeinfo->tm_wday = bcd_to_dec(data[3] & 0x07) - 1;
    timeinfo->tm_mday = bcd_to_dec(data[4] & 0x3F);
    timeinfo->tm_mon = bcd_to_dec(data[5] & 0x1F) - 1;
    timeinfo->tm_year = bcd_to_dec(data[6]) + 100;

    ESP_LOGD(TAG, "Read time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    return ESP_OK;
}

esp_err_t ds1307_write_time(const struct tm *timeinfo) {
    if (!ds1307_handle || !timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[8];
    data[0] = DS1307_REG_SECONDS;
    data[1] = dec_to_bcd(timeinfo->tm_sec) & 0x7F;
    data[2] = dec_to_bcd(timeinfo->tm_min);
    data[3] = dec_to_bcd(timeinfo->tm_hour) & 0x3F;
    data[4] = dec_to_bcd(timeinfo->tm_wday + 1);
    data[5] = dec_to_bcd(timeinfo->tm_mday);
    data[6] = dec_to_bcd(timeinfo->tm_mon + 1);
    data[7] = dec_to_bcd(timeinfo->tm_year - 100);

    esp_err_t ret = i2c_dev_support_write(ds1307_handle, data, 8, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to DS1307: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Time written to DS1307: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    return ESP_OK;
}

esp_err_t ds1307_is_running(bool *is_running) {
    if (!ds1307_handle || !is_running) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = DS1307_REG_SECONDS;
    uint8_t seconds;
    esp_err_t ret = i2c_dev_support_write_read(ds1307_handle, &reg_addr, 1, &seconds, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    *is_running = ((seconds & 0x80) == 0);
    return ESP_OK;
}

esp_err_t ds1307_start(void) {
    if (!ds1307_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr = DS1307_REG_SECONDS;
    uint8_t seconds;
    esp_err_t ret = i2c_dev_support_write_read(ds1307_handle, &reg_addr, 1, &seconds, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    seconds &= 0x7F;
    uint8_t data[2] = {DS1307_REG_SECONDS, seconds};
    ret = i2c_dev_support_write(ds1307_handle, data, 2, 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DS1307 clock started");
    }
    return ret;
}

esp_err_t ds1307_stop(void) {
    if (!ds1307_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr = DS1307_REG_SECONDS;
    uint8_t seconds;
    esp_err_t ret = i2c_dev_support_write_read(ds1307_handle, &reg_addr, 1, &seconds, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    seconds |= 0x80;
    uint8_t data[2] = {DS1307_REG_SECONDS, seconds};
    ret = i2c_dev_support_write(ds1307_handle, data, 2, 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DS1307 clock stopped");
    }
    return ret;
}
