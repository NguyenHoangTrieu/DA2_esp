/**
 * @file ds1307_rtc.c
 * @brief DS1307 RTC Driver Implementation using ESP-IDF 6.0 I2C Master API
 */

#include "ds1307_rtc.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DS1307_RTC";

// I2C master handle
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t ds1307_handle = NULL;

// DS1307 Register addresses
#define DS1307_REG_SECONDS 0x00
#define DS1307_REG_MINUTES 0x01
#define DS1307_REG_HOURS 0x02
#define DS1307_REG_DAY 0x03
#define DS1307_REG_DATE 0x04
#define DS1307_REG_MONTH 0x05
#define DS1307_REG_YEAR 0x06
#define DS1307_REG_CONTROL 0x07

// Helper: BCD to Decimal
static uint8_t bcd_to_dec(uint8_t val) {
  return (val & 0x0F) + ((val >> 4) * 10);
}

// Helper: Decimal to BCD
static uint8_t dec_to_bcd(uint8_t val) {
  return ((val / 10) << 4) | (val % 10);
}

esp_err_t ds1307_init(void) {
  esp_err_t ret;

  ESP_LOGI(TAG, "Initializing DS1307 RTC on I2C%d (SDA=%d, SCL=%d)",
           DS1307_I2C_PORT, DS1307_I2C_SDA_PIN, DS1307_I2C_SCL_PIN);

  // Configure I2C master bus
  i2c_master_bus_config_t bus_config = {
      .i2c_port = DS1307_I2C_PORT,
      .sda_io_num = DS1307_I2C_SDA_PIN,
      .scl_io_num = DS1307_I2C_SCL_PIN,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };

  ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
    return ret;
  }

  // Add DS1307 device to bus
  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = DS1307_I2C_ADDR,
      .scl_speed_hz = DS1307_I2C_FREQ_HZ,
  };

  ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &ds1307_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add DS1307 device: %s", esp_err_to_name(ret));
    i2c_del_master_bus(i2c_bus_handle);
    i2c_bus_handle = NULL;
    return ret;
  }

  // Verify DS1307 is present by reading control register
  uint8_t reg_addr = DS1307_REG_CONTROL;
  uint8_t control_val;
  ret = i2c_master_transmit_receive(ds1307_handle, &reg_addr, 1, &control_val,
                                    1, pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "DS1307 not found at address 0x%02X", DS1307_I2C_ADDR);
    i2c_master_bus_rm_device(ds1307_handle);
    i2c_del_master_bus(i2c_bus_handle);
    ds1307_handle = NULL;
    i2c_bus_handle = NULL;
    return ESP_ERR_NOT_FOUND;
  }

  // Check if clock is running
  bool is_running;
  ret = ds1307_is_running(&is_running);
  if (ret == ESP_OK) {
    if (!is_running) {
      ESP_LOGW(TAG, "DS1307 clock halted, starting...");
      ds1307_start();
    }
  }

  ESP_LOGI(TAG, "DS1307 RTC initialized successfully");
  return ESP_OK;
}

esp_err_t ds1307_deinit(void) {
  if (ds1307_handle) {
    i2c_master_bus_rm_device(ds1307_handle);
    ds1307_handle = NULL;
  }
  if (i2c_bus_handle) {
    i2c_del_master_bus(i2c_bus_handle);
    i2c_bus_handle = NULL;
  }
  ESP_LOGI(TAG, "DS1307 RTC deinitialized");
  return ESP_OK;
}

esp_err_t ds1307_read_time(struct tm *timeinfo) {
  if (!ds1307_handle || !timeinfo) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t reg_addr = DS1307_REG_SECONDS;
  uint8_t data[7];

  // Read 7 bytes starting from seconds register
  esp_err_t ret = i2c_master_transmit_receive(ds1307_handle, &reg_addr, 1, data,
                                              7, pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read time from DS1307: %s", esp_err_to_name(ret));
    return ret;
  }

  // Parse BCD data
  timeinfo->tm_sec = bcd_to_dec(data[0] & 0x7F); // Mask CH bit
  timeinfo->tm_min = bcd_to_dec(data[1] & 0x7F);
  timeinfo->tm_hour = bcd_to_dec(data[2] & 0x3F);     // 24-hour format
  timeinfo->tm_wday = bcd_to_dec(data[3] & 0x07) - 1; // 0-6 (Sun-Sat)
  timeinfo->tm_mday = bcd_to_dec(data[4] & 0x3F);
  timeinfo->tm_mon = bcd_to_dec(data[5] & 0x1F) - 1; // 0-11
  timeinfo->tm_year = bcd_to_dec(data[6]) + 100;     // Years since 1900

  // Validate year range
  if (timeinfo->tm_year < 100 || timeinfo->tm_year > 199) {
    ESP_LOGW(TAG, "Invalid year from DS1307: %d", timeinfo->tm_year + 1900);
    return ESP_ERR_INVALID_RESPONSE;
  }

  ESP_LOGD(TAG, "Read time: %04d-%02d-%02d %02d:%02d:%02d",
           timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
           timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

  return ESP_OK;
}

esp_err_t ds1307_write_time(const struct tm *timeinfo) {
  if (!ds1307_handle || !timeinfo) {
    return ESP_ERR_INVALID_ARG;
  }

  // Prepare data: [register_address][7 time bytes]
  uint8_t data[8];
  data[0] = DS1307_REG_SECONDS;
  data[1] = dec_to_bcd(timeinfo->tm_sec) & 0x7F; // Clear CH bit (enable clock)
  data[2] = dec_to_bcd(timeinfo->tm_min);
  data[3] = dec_to_bcd(timeinfo->tm_hour) & 0x3F; // 24-hour format
  data[4] = dec_to_bcd(timeinfo->tm_wday + 1);    // 1-7
  data[5] = dec_to_bcd(timeinfo->tm_mday);
  data[6] = dec_to_bcd(timeinfo->tm_mon + 1);    // 1-12
  data[7] = dec_to_bcd(timeinfo->tm_year - 100); // 00-99 (2000-2099)

  esp_err_t ret =
      i2c_master_transmit(ds1307_handle, data, 8, pdMS_TO_TICKS(1000));
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

  esp_err_t ret = i2c_master_transmit_receive(ds1307_handle, &reg_addr, 1,
                                              &seconds, 1, pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) {
    return ret;
  }

  // CH bit is bit 7 of seconds register
  *is_running = ((seconds & 0x80) == 0);
  return ESP_OK;
}

esp_err_t ds1307_start(void) {
  if (!ds1307_handle) {
    return ESP_ERR_INVALID_STATE;
  }

  // Read current seconds value
  uint8_t reg_addr = DS1307_REG_SECONDS;
  uint8_t seconds;

  esp_err_t ret = i2c_master_transmit_receive(ds1307_handle, &reg_addr, 1,
                                              &seconds, 1, pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) {
    return ret;
  }

  // Clear CH bit (bit 7)
  seconds &= 0x7F;

  // Write back
  uint8_t data[2] = {DS1307_REG_SECONDS, seconds};
  ret = i2c_master_transmit(ds1307_handle, data, 2, pdMS_TO_TICKS(1000));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "DS1307 clock started");
  }
  return ret;
}

esp_err_t ds1307_stop(void) {
  if (!ds1307_handle) {
    return ESP_ERR_INVALID_STATE;
  }

  // Read current seconds value
  uint8_t reg_addr = DS1307_REG_SECONDS;
  uint8_t seconds;

  esp_err_t ret = i2c_master_transmit_receive(ds1307_handle, &reg_addr, 1,
                                              &seconds, 1, pdMS_TO_TICKS(1000));
  if (ret != ESP_OK) {
    return ret;
  }

  // Set CH bit (bit 7)
  seconds |= 0x80;

  // Write back
  uint8_t data[2] = {DS1307_REG_SECONDS, seconds};
  ret = i2c_master_transmit(ds1307_handle, data, 2, pdMS_TO_TICKS(1000));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "DS1307 clock stopped");
  }
  return ret;
}
