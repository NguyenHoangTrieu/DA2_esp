/**
 * @file ina230_handler.c
 * @brief INA230AIRGTR Power Monitor I2C Driver Implementation
 */

#include "ina230_handler.h"
#include "esp_log.h"

static const char *TAG = "INA230";

static i2c_master_dev_handle_t s_dev = NULL;

/* Calibration constants (R_shunt = 10mΩ, current_lsb = 0.2mA/bit)
 *   CAL = 0.00512 / (current_lsb × R_shunt)
 *       = 0.00512 / (0.0002 × 0.010) = 2560
 *   Max measurable current = 32767 × 0.2mA = 6553mA ≈ 6.55A
 *   Power_LSB = 25 × 0.2mA = 5mW/bit
 */
#define INA230_CALIBRATION_VAL  2560
#define INA230_CURRENT_LSB_UMA  200     /* 0.2mA = 200μA per bit */
#define INA230_POWER_LSB_UW     5000    /* 5mW = 5000μW per bit */

/* CONFIG = AVG=001(4), VBUSCT=100(1.1ms), VSHCT=100(1.1ms), MODE=111 (shunt+bus continuous)
 * Bits [11:9]=001, [8:6]=100, [5:3]=100, [2:0]=111
 * = 0b0000_001_100_100_111 = 0x0327 */
#define INA230_CONFIG_VAL   0x0327

/* ------------------------------------------------------------------ */

static esp_err_t reg_read16(uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_dev_support_write_read(s_dev, &reg, 1, buf, 2, 50);
    if (ret == ESP_OK) {
        *val = ((uint16_t)buf[0] << 8) | buf[1];  /* big-endian */
    }
    return ret;
}

static esp_err_t reg_write16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_dev_support_write(s_dev, buf, 3, 50);
}

/* ------------------------------------------------------------------ */

esp_err_t ina230_init(void)
{
    esp_err_t ret = i2c_dev_support_add_device(INA230_I2C_ADDR,
                                               INA230_I2C_FREQ_HZ,
                                               &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device (addr=0x%02X): %s",
                 INA230_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* Verify manufacturer ID (0x5449 = "TI") */
    uint16_t mfr = 0;
    ret = reg_read16(INA230_REG_MANUFACTURER, &mfr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INA230 not responding: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "INA230 found (MFR=0x%04X)", mfr);

    /* Write configuration */
    ret = reg_write16(INA230_REG_CONFIG, INA230_CONFIG_VAL);
    if (ret != ESP_OK) goto fail;

    /* Write calibration */
    ret = reg_write16(INA230_REG_CALIBRATION, INA230_CALIBRATION_VAL);
    if (ret != ESP_OK) goto fail;

    ESP_LOGI(TAG, "Initialized: R_shunt=10mΩ, current_lsb=0.2mA, CAL=%u",
             INA230_CALIBRATION_VAL);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t ina230_read_bus_voltage_mv(uint16_t *voltage_mv)
{
    if (!s_dev || !voltage_mv) return ESP_ERR_INVALID_ARG;
    uint16_t raw;
    esp_err_t ret = reg_read16(INA230_REG_BUS_V, &raw);
    if (ret == ESP_OK) {
        /* Bus voltage LSB = 1.25mV; scale ×5 / 4 to avoid float */
        *voltage_mv = (uint16_t)((uint32_t)raw * 5u / 4u);
    }
    return ret;
}

esp_err_t ina230_read_current_ma(int16_t *current_ma)
{
    if (!s_dev || !current_ma) return ESP_ERR_INVALID_ARG;
    uint16_t raw;
    esp_err_t ret = reg_read16(INA230_REG_CURRENT, &raw);
    if (ret == ESP_OK) {
        /* Signed 16-bit, current_lsb = 0.2mA → multiply by 200μA / 1000 */
        int16_t signed_raw = (int16_t)raw;
        *current_ma = (int16_t)((int32_t)signed_raw * INA230_CURRENT_LSB_UMA / 1000);
    }
    return ret;
}

esp_err_t ina230_read_status(ina230_status_t *status)
{
    if (!s_dev || !status) return ESP_ERR_INVALID_ARG;

    uint16_t bus_raw, cur_raw, pwr_raw;
    esp_err_t ret;

    ret = reg_read16(INA230_REG_BUS_V, &bus_raw);
    if (ret != ESP_OK) return ret;
    ret = reg_read16(INA230_REG_CURRENT, &cur_raw);
    if (ret != ESP_OK) return ret;
    ret = reg_read16(INA230_REG_POWER, &pwr_raw);
    if (ret != ESP_OK) return ret;

    status->bus_voltage_mv = (uint16_t)((uint32_t)bus_raw * 5u / 4u);
    status->current_ma     = (int16_t)((int32_t)(int16_t)cur_raw
                                       * INA230_CURRENT_LSB_UMA / 1000);
    /* Power register is always positive; power_lsb = 5mW/bit */
    status->power_mw       = (int32_t)pwr_raw * (int32_t)(INA230_POWER_LSB_UW / 1000);

    return ESP_OK;
}
