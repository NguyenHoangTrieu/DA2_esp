/**
 * @file bq27441_handler.c
 * @brief BQ27441DRZR-G1B Battery Fuel Gauge I2C Driver Implementation
 */

#include "bq27441_handler.h"
#include "esp_log.h"

static const char *TAG = "BQ27441";

static i2c_master_dev_handle_t s_dev = NULL;

/* ------------------------------------------------------------------ */
/*  Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Read a 16-bit standard command value (little-endian).
 */
static esp_err_t cmd_read16(uint8_t cmd, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_dev_support_write_read(s_dev, &cmd, 1, buf, 2, 50);
    if (ret == ESP_OK) {
        *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);  /* little-endian */
    }
    return ret;
}

/**
 * @brief Write a 16-bit sub-command to the Control() register.
 */
static esp_err_t ctrl_write(uint16_t sub_cmd)
{
    uint8_t buf[3] = {
        BQ27441_CMD_CONTROL,
        (uint8_t)(sub_cmd & 0xFF),
        (uint8_t)(sub_cmd >> 8)
    };
    return i2c_dev_support_write(s_dev, buf, 3, 50);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t bq27441_init(void)
{
    esp_err_t ret = i2c_dev_support_add_device(BQ27441_I2C_ADDR,
                                               BQ27441_I2C_FREQ_HZ,
                                               &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device (addr=0x%02X): %s",
                 BQ27441_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* Read device type via Control(DEVICE_TYPE) → returns 0x0421 for BQ27441-G1 */
    ret = ctrl_write(BQ27441_CTRL_DEVICE_TYPE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BQ27441 not responding: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t dev_type = 0;
    ret = cmd_read16(BQ27441_CMD_CONTROL, &dev_type);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device type: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BQ27441 found (device type=0x%04X)", dev_type);

    /* Read initial status */
    uint16_t flags = 0;
    cmd_read16(BQ27441_CMD_FLAGS, &flags);
    bool bat_present = (flags & BQ27441_FLAG_BAT_DET) != 0;
    ESP_LOGI(TAG, "Battery %s", bat_present ? "detected" : "NOT detected");

    return ESP_OK;
}

esp_err_t bq27441_read_voltage_mv(uint16_t *voltage_mv)
{
    if (!s_dev || !voltage_mv) return ESP_ERR_INVALID_ARG;
    return cmd_read16(BQ27441_CMD_VOLTAGE, voltage_mv);
}

esp_err_t bq27441_read_soc_pct(uint8_t *soc_pct)
{
    if (!s_dev || !soc_pct) return ESP_ERR_INVALID_ARG;
    uint16_t raw;
    esp_err_t ret = cmd_read16(BQ27441_CMD_SOC, &raw);
    if (ret == ESP_OK) {
        *soc_pct = (raw > 100) ? 100 : (uint8_t)raw;
    }
    return ret;
}

esp_err_t bq27441_read_flags(uint16_t *flags)
{
    if (!s_dev || !flags) return ESP_ERR_INVALID_ARG;
    return cmd_read16(BQ27441_CMD_FLAGS, flags);
}

esp_err_t bq27441_read_avg_current_ma(int16_t *current_ma)
{
    if (!s_dev || !current_ma) return ESP_ERR_INVALID_ARG;
    uint16_t raw;
    esp_err_t ret = cmd_read16(BQ27441_CMD_AVG_CURRENT, &raw);
    if (ret == ESP_OK) {
        *current_ma = (int16_t)raw;
    }
    return ret;
}

esp_err_t bq27441_read_status(bq27441_status_t *status)
{
    if (!s_dev || !status) return ESP_ERR_INVALID_ARG;

    uint16_t raw_v, raw_soc, raw_cur, raw_pwr, raw_flags;
    esp_err_t ret;

    ret = cmd_read16(BQ27441_CMD_VOLTAGE,     &raw_v);    if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_SOC,         &raw_soc);  if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_AVG_CURRENT, &raw_cur);  if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_AVG_POWER,   &raw_pwr);  if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_FLAGS,       &raw_flags); if (ret != ESP_OK) return ret;

    status->voltage_mv      = raw_v;
    status->soc_pct         = (raw_soc > 100) ? 100 : (uint8_t)raw_soc;
    status->avg_current_ma  = (int16_t)raw_cur;
    status->avg_power_mw    = (int16_t)raw_pwr;
    status->flags           = raw_flags;
    status->fully_charged   = (raw_flags & BQ27441_FLAG_FC)      != 0;
    status->critical_low    = (raw_flags & BQ27441_FLAG_SOCF)    != 0;
    status->battery_present = (raw_flags & BQ27441_FLAG_BAT_DET) != 0;

    return ESP_OK;
}
