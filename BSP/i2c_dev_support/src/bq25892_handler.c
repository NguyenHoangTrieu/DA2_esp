/**
 * @file bq25892_handler.c
 * @brief BQ25892RTWR Battery Charger I2C Driver Implementation
 */

#include "bq25892_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BQ25892";

static i2c_master_dev_handle_t s_dev = NULL;

/* ------------------------------------------------------------------ */
/*  Low-level register helpers                                          */
/* ------------------------------------------------------------------ */

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_dev_support_write_read(s_dev, &reg, 1, val, 1, 50);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_dev_support_write(s_dev, buf, 2, 50);
}

static esp_err_t reg_modify(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    val = (val & ~mask) | (bits & mask);
    return reg_write(reg, val);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t bq25892_init(void)
{
    esp_err_t ret = i2c_dev_support_add_device(BQ25892_I2C_ADDR,
                                               BQ25892_I2C_FREQ_HZ,
                                               &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device (addr=0x%02X): %s",
                 BQ25892_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* Verify device presence */
    uint8_t dev_id;
    ret = reg_read(BQ25892_REG14, &dev_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BQ25892 not responding: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "BQ25892 found (REG14=0x%02X)", dev_id);

    /* REG00: EN_ILIM=1 (use pin limit), IINLIM=58 → 3000mA
     *   IINLIM formula: I(mA) = 100 + IINLIM×50  →  3000 = 100 + 58×50  → IINLIM=58 */
    ret = reg_write(BQ25892_REG00, 0x7A);   /* 0b0111_1010 */
    if (ret != ESP_OK) goto fail;

    /* REG06: VREG=16 (4096mV ≈ 4.1V), BATLOWV=1 (3.0V pre-charge threshold)
     *   REG06[7:2] = VREG[5:0] = 16 = 0b010000
     *   REG06[1]   = BATLOWV = 1
     *   REG06 = (16 << 2) | 0x02 = 0x42 */
    ret = reg_write(BQ25892_REG06, 0x42);
    if (ret != ESP_OK) goto fail;

    /* REG03: CHG_CONFIG=1 (enable charging), SYS_MIN=3.5V (101), OTG=0
     *   SYS_MIN=101 → bits[3:1] = 0b101 = 0x0A; CHG_CONFIG=1 → bit4 = 0x10
     *   REG03 = 0b0001_1010 = 0x1A */
    ret = reg_write(BQ25892_REG03, 0x1A);
    if (ret != ESP_OK) goto fail;

    ESP_LOGI(TAG, "Initialized: IINLIM=3A, VREG=4096mV (4.1V), charging enabled");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bq25892_set_charge_enable(bool enable)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = reg_modify(BQ25892_REG03,
                               BQ25892_CHG_CONFIG_BIT,
                               enable ? BQ25892_CHG_CONFIG_BIT : 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Charging %s", enable ? "ENABLED" : "DISABLED");
    }
    return ret;
}

esp_err_t bq25892_set_charge_voltage_mv(uint16_t vreg_mv)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (vreg_mv < 3840) vreg_mv = 3840;
    if (vreg_mv > 4608) vreg_mv = 4608;
    uint8_t vreg = (uint8_t)((vreg_mv - 3840) / 16);
    /* Read current REG06, preserve BATLOWV and VRECHG bits */
    uint8_t cur;
    esp_err_t ret = reg_read(BQ25892_REG06, &cur);
    if (ret != ESP_OK) return ret;
    cur = (cur & 0x03) | (uint8_t)(vreg << 2);
    ret = reg_write(BQ25892_REG06, cur);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Charge voltage set to %u mV (VREG=%u)", vreg_mv, vreg);
    }
    return ret;
}

esp_err_t bq25892_set_otg(bool enable)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return reg_modify(BQ25892_REG03,
                      BQ25892_OTG_CONFIG_BIT,
                      enable ? BQ25892_OTG_CONFIG_BIT : 0);
}

esp_err_t bq25892_read_batv_mv(uint16_t *vbat_mv)
{
    if (!s_dev || !vbat_mv) return ESP_ERR_INVALID_ARG;

    /* Trigger single ADC conversion: CONV_START=1, CONV_RATE=0 (one-shot) */
    esp_err_t ret = reg_modify(BQ25892_REG02, 0xC0, 0x80);
    if (ret != ESP_OK) return ret;

    /* Wait for conversion (typically <1ms per channel, conservatively 10ms) */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t raw;
    ret = reg_read(BQ25892_REG0E, &raw);
    if (ret != ESP_OK) return ret;

    /* BATV[6:0] = REG0E[6:0]; voltage_mv = 2304 + BATV × 20 */
    *vbat_mv = 2304u + (uint16_t)((raw & 0x7F) * 20u);
    return ESP_OK;
}

esp_err_t bq25892_read_status(bq25892_status_t *status)
{
    if (!s_dev || !status) return ESP_ERR_INVALID_ARG;

    uint8_t reg0b, reg09, reg03;
    esp_err_t ret = reg_read(BQ25892_REG0B, &reg0b);
    if (ret != ESP_OK) return ret;
    ret = reg_read(BQ25892_REG09, &reg09);
    if (ret != ESP_OK) return ret;
    ret = reg_read(BQ25892_REG03, &reg03);
    if (ret != ESP_OK) return ret;

    uint8_t chrg = reg0b & BQ25892_CHRG_STAT_MASK;
    switch (chrg) {
        case BQ25892_CHRG_STAT_PRE_CHG:  status->chrg_status = BQ25892_STATUS_PRE_CHARGING;  break;
        case BQ25892_CHRG_STAT_FAST_CHG: status->chrg_status = BQ25892_STATUS_FAST_CHARGING; break;
        case BQ25892_CHRG_STAT_DONE:     status->chrg_status = BQ25892_STATUS_CHARGE_DONE;    break;
        default:                          status->chrg_status = BQ25892_STATUS_NOT_CHARGING;   break;
    }

    status->power_good     = (reg0b & 0x04) != 0;   /* PG_STAT bit[2] */
    status->charge_enabled = (reg03 & BQ25892_CHG_CONFIG_BIT) != 0;
    status->fault_reg      = reg09;

    /* Optional: also read BATV if ADC was already triggered */
    ret = bq25892_read_batv_mv(&status->vbat_mv);
    if (ret != ESP_OK) {
        status->vbat_mv = 0;
        ret = ESP_OK;   /* non-fatal */
    }

    return ESP_OK;
}
