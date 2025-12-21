/**
 * @file pcf8563_rtc.c
 * @brief PCF8563 RTC Driver Implementation
 */

#include "pcf8563_rtc.h"
#include "i2c_dev_support.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "PCF8563_RTC";
static i2c_master_dev_handle_t pcf8563_handle = NULL;

// PCF8563 Register addresses
#define PCF8563_REG_CTRL_STATUS1  0x00
#define PCF8563_REG_CTRL_STATUS2  0x01
#define PCF8563_REG_VL_SECONDS    0x02
#define PCF8563_REG_MINUTES       0x03
#define PCF8563_REG_HOURS         0x04
#define PCF8563_REG_DAYS          0x05
#define PCF8563_REG_WEEKDAYS      0x06
#define PCF8563_REG_CENTURY_MONTH 0x07
#define PCF8563_REG_YEARS         0x08

// Control/Status bits
#define PCF8563_CTRL1_STOP    (1 << 5)
#define PCF8563_CTRL1_TESTC   (1 << 3)
#define PCF8563_SECONDS_VL    (1 << 7)
#define PCF8563_MONTH_CENTURY (1 << 7)

// Helper: BCD to Decimal
static uint8_t bcd_to_dec(uint8_t val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

// Helper: Decimal to BCD
static uint8_t dec_to_bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

esp_err_t pcf8563_init(void) {
    // Check if I2C support is initialized
    if (!i2c_dev_support_is_initialized()) {
        ESP_LOGE(TAG, "I2C not initialized. Call i2c_dev_support_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing PCF8563 RTC at address 0x%02X", PCF8563_I2C_ADDR);

    // Add PCF8563 device to bus
    esp_err_t ret = i2c_dev_support_add_device(PCF8563_I2C_ADDR, PCF8563_I2C_FREQ_HZ, &pcf8563_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCF8563 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify PCF8563 is present by reading control register
    uint8_t reg_addr = PCF8563_REG_CTRL_STATUS1;
    uint8_t control_val;
    ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &control_val, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF8563 not found at address 0x%02X", PCF8563_I2C_ADDR);
        i2c_dev_support_remove_device(pcf8563_handle);
        pcf8563_handle = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // Clear TESTC bit and ensure normal operation
    if (control_val & PCF8563_CTRL1_TESTC) {
        uint8_t data[2] = {PCF8563_REG_CTRL_STATUS1, control_val & ~PCF8563_CTRL1_TESTC};
        i2c_dev_support_write(pcf8563_handle, data, 2, 1000);
    }

    // Check if clock is running
    bool is_running;
    ret = pcf8563_is_running(&is_running);
    if (ret == ESP_OK && !is_running) {
        ESP_LOGW(TAG, "PCF8563 clock halted, starting...");
        pcf8563_start();
    }

    // Check for voltage low condition
    bool voltage_low;
    if (pcf8563_check_voltage_low(&voltage_low) == ESP_OK && voltage_low) {
        ESP_LOGW(TAG, "PCF8563 voltage low detected - time may be invalid");
    }

    ESP_LOGI(TAG, "PCF8563 RTC initialized successfully");
    return ESP_OK;
}

esp_err_t pcf8563_deinit(void) {
    if (pcf8563_handle) {
        esp_err_t ret = i2c_dev_support_remove_device(pcf8563_handle);
        pcf8563_handle = NULL;
        ESP_LOGI(TAG, "PCF8563 RTC deinitialized");
        return ret;
    }
    return ESP_OK;
}

esp_err_t pcf8563_read_time(struct tm *timeinfo) {
    if (!pcf8563_handle || !timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = PCF8563_REG_VL_SECONDS;
    uint8_t data[7];
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, data, 7, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from PCF8563: %s", esp_err_to_name(ret));
        return ret;
    }

    // Parse BCD data
    timeinfo->tm_sec  = bcd_to_dec(data[0] & 0x7F);  // Mask VL bit
    timeinfo->tm_min  = bcd_to_dec(data[1] & 0x7F);
    timeinfo->tm_hour = bcd_to_dec(data[2] & 0x3F);
    timeinfo->tm_mday = bcd_to_dec(data[3] & 0x3F);
    timeinfo->tm_wday = bcd_to_dec(data[4] & 0x07);  // 0-6
    
    // Handle century bit and month
    uint8_t month = bcd_to_dec(data[5] & 0x1F);
    bool century = (data[5] & PCF8563_MONTH_CENTURY) ? true : false;
    timeinfo->tm_mon  = month - 1;  // tm_mon is 0-11
    
    // Calculate year
    uint8_t year = bcd_to_dec(data[6]);
    if (century) {
        timeinfo->tm_year = year + 100;  // 2000-2099 (tm_year = years since 1900)
    } else {
        timeinfo->tm_year = year;  // 1900-1999
    }

    ESP_LOGD(TAG, "Read time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

    return ESP_OK;
}

esp_err_t pcf8563_write_time(const struct tm *timeinfo) {
    if (!pcf8563_handle || !timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[8];
    data[0] = PCF8563_REG_VL_SECONDS;
    data[1] = dec_to_bcd(timeinfo->tm_sec) & 0x7F;  // Clear VL bit
    data[2] = dec_to_bcd(timeinfo->tm_min) & 0x7F;
    data[3] = dec_to_bcd(timeinfo->tm_hour) & 0x3F;
    data[4] = dec_to_bcd(timeinfo->tm_mday) & 0x3F;
    data[5] = dec_to_bcd(timeinfo->tm_wday) & 0x07;
    
    // Set century bit if year >= 2000
    uint8_t month = dec_to_bcd(timeinfo->tm_mon + 1);
    if (timeinfo->tm_year >= 100) {  // Year 2000 or later
        data[6] = month | PCF8563_MONTH_CENTURY;
        data[7] = dec_to_bcd(timeinfo->tm_year - 100);
    } else {
        data[6] = month & 0x1F;
        data[7] = dec_to_bcd(timeinfo->tm_year);
    }

    esp_err_t ret = i2c_dev_support_write(pcf8563_handle, data, 8, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to PCF8563: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Time written to PCF8563: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

    return ESP_OK;
}

esp_err_t pcf8563_is_running(bool *is_running) {
    if (!pcf8563_handle || !is_running) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = PCF8563_REG_CTRL_STATUS1;
    uint8_t ctrl_status1;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &ctrl_status1, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    *is_running = ((ctrl_status1 & PCF8563_CTRL1_STOP) == 0);
    return ESP_OK;
}

esp_err_t pcf8563_start(void) {
    if (!pcf8563_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr = PCF8563_REG_CTRL_STATUS1;
    uint8_t ctrl_status1;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &ctrl_status1, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    ctrl_status1 &= ~PCF8563_CTRL1_STOP;  // Clear STOP bit
    uint8_t data[2] = {PCF8563_REG_CTRL_STATUS1, ctrl_status1};
    
    ret = i2c_dev_support_write(pcf8563_handle, data, 2, 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PCF8563 clock started");
    }

    return ret;
}

esp_err_t pcf8563_stop(void) {
    if (!pcf8563_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr = PCF8563_REG_CTRL_STATUS1;
    uint8_t ctrl_status1;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &ctrl_status1, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    ctrl_status1 |= PCF8563_CTRL1_STOP;  // Set STOP bit
    uint8_t data[2] = {PCF8563_REG_CTRL_STATUS1, ctrl_status1};
    
    ret = i2c_dev_support_write(pcf8563_handle, data, 2, 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PCF8563 clock stopped");
    }

    return ret;
}

esp_err_t pcf8563_check_voltage_low(bool *is_low) {
    if (!pcf8563_handle || !is_low) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = PCF8563_REG_VL_SECONDS;
    uint8_t seconds;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &seconds, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    *is_low = ((seconds & PCF8563_SECONDS_VL) != 0);
    return ESP_OK;
}

esp_err_t pcf8563_clear_voltage_low(void) {
    if (!pcf8563_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr = PCF8563_REG_VL_SECONDS;
    uint8_t seconds;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, &reg_addr, 1, &seconds, 1, 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    seconds &= ~PCF8563_SECONDS_VL;  // Clear VL bit
    uint8_t data[2] = {PCF8563_REG_VL_SECONDS, seconds};
    
    ret = i2c_dev_support_write(pcf8563_handle, data, 2, 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PCF8563 voltage low flag cleared");
    }

    return ret;
}

/**
 * @brief Clear voltage low flag in PCF8563
 * 
 * This function clears the VL (Voltage Low) bit in the seconds register.
 * The VL bit is set by the PCF8563 when power drops below a threshold,
 * indicating that the time data may be invalid.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t pcf8563_clear_voltage_low_flag(void) {
    if (!pcf8563_handle) {
        ESP_LOGE(TAG, "PCF8563 not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Read current seconds register (contains VL bit)
    uint8_t reg_addr = PCF8563_REG_VL_SECONDS;
    uint8_t seconds_reg;
    
    esp_err_t ret = i2c_dev_support_write_read(pcf8563_handle, 
                                                &reg_addr, 1, 
                                                &seconds_reg, 1, 
                                                1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read seconds register: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Check if VL bit is set
    if (!(seconds_reg & PCF8563_SECONDS_VL)) {
        ESP_LOGD(TAG, "Voltage low flag already clear");
        return ESP_OK;
    }
    
    // Clear VL bit (bit 7) while preserving seconds value
    seconds_reg &= ~PCF8563_SECONDS_VL;
    
    // Write back the modified register
    uint8_t data[2] = {PCF8563_REG_VL_SECONDS, seconds_reg};
    ret = i2c_dev_support_write(pcf8563_handle, data, 2, 1000);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Voltage low flag cleared successfully");
    } else {
        ESP_LOGE(TAG, "Failed to clear voltage low flag: %s", esp_err_to_name(ret));
    }
    
    return ret;
}