/**
 * @file bq27441_handler.c
 * @brief BQ27441DRZR-G1B Battery Fuel Gauge I2C Driver Implementation
 *
 * FIX LOG:
 *  [FIX-1] BQ27441_DF_CLASS_STATE = 0x52 (subclass 82 "State") replaces
 *          the wrong BQ27441_DF_CLASS_POWER constant. DESIGN_CAPACITY and
 *          DESIGN_ENERGY live in subclass 82, not any "Power" subclass.
 *
 *  [FIX-2] DESIGN_ENERGY = capacity_mah * 37 / 10 (≈ 3.7 V nominal),
 *          not capacity_mah (which implied ≈ 1 V, a 3.7× error).
 *
 *  [FIX-3] Checksum computed from the local modified block[] immediately
 *          after modification — no read from 0x60, no block re-read.
 *          Reading 0x60 mid-sequence resets the BQ27441's internal block
 *          data state machine, causing a NACK on the subsequent write to
 *          0x60 (observed as ESP_ERR_INVALID_RESPONSE from i2c_master_transmit).
 *
 *  [FIX-4] Post-write verification and bq27441_read_design_capacity()
 *          both use BQ27441_DF_CLASS_STATE.
 */

#include "bq27441_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BQ27441";

static i2c_master_dev_handle_t s_dev = NULL;

/* ------------------------------------------------------------------ */
/* Data flash subclass constants                                        */
/* ------------------------------------------------------------------ */

/*
 * Subclass 82 / 0x52 = "State"  (TRM SLUUAC9, Table 17)
 *   Offset 10-11 : Design Capacity  (big-endian, mAh, default 1000)
 *   Offset 12-13 : Design Energy    (big-endian, mWh, default 3700)
 *
 * NOT subclass 34 / 0x22 = "Power" (charge/discharge thresholds).
 */
#define BQ27441_DF_CLASS_STATE   0x52u   /* [FIX-1] */

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_read16(uint8_t cmd, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_dev_support_write_read(s_dev, &cmd, 1, buf, 2, 50);
    if (ret == ESP_OK)
        *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return ret;
}

static esp_err_t ctrl_write(uint16_t sub_cmd)
{
    uint8_t buf[3] = {BQ27441_CMD_CONTROL,
                      (uint8_t)(sub_cmd & 0xFF),
                      (uint8_t)(sub_cmd >> 8)};
    return i2c_dev_support_write(s_dev, buf, 3, 50);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t bq27441_init(void)
{
    esp_err_t ret = i2c_dev_support_add_device(BQ27441_I2C_ADDR,
                                               BQ27441_I2C_FREQ_HZ, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device (addr=0x%02X): %s",
                 BQ27441_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

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

    uint16_t flags = 0;
    cmd_read16(BQ27441_CMD_FLAGS, &flags);
    ESP_LOGI(TAG, "Battery %s",
             (flags & BQ27441_FLAG_BAT_DET) ? "detected" : "NOT detected");

    uint16_t nom_cap = 0;
    cmd_read16(BQ27441_CMD_NOM_CAP, &nom_cap);
    ESP_LOGI(TAG, "Nominal Capacity: %u mAh", nom_cap);

    if (nom_cap != 3000)
        ESP_LOGW(TAG, "Nominal capacity is %u mAh, expected 3000 mAh. "
                      "SoC may be inaccurate.", nom_cap);

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
    if (ret == ESP_OK)
        *soc_pct = (raw > 100) ? 100 : (uint8_t)raw;
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
    if (ret == ESP_OK)
        *current_ma = (int16_t)raw;
    return ret;
}

esp_err_t bq27441_read_status(bq27441_status_t *status)
{
    if (!s_dev || !status) return ESP_ERR_INVALID_ARG;

    uint16_t raw_v, raw_soc, raw_cur, raw_pwr, raw_flags;
    esp_err_t ret;

    ret = cmd_read16(BQ27441_CMD_VOLTAGE,     &raw_v);     if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_SOC,          &raw_soc);   if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_AVG_CURRENT,  &raw_cur);   if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_AVG_POWER,    &raw_pwr);   if (ret != ESP_OK) return ret;
    ret = cmd_read16(BQ27441_CMD_FLAGS,        &raw_flags); if (ret != ESP_OK) return ret;

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

/* ================================================================== */
/* Data Flash — Capacity Reprogramming                                  */
/* ================================================================== */

/*
 * BQ27441-G1 CFGUPDATE block-data write sequence (TRM SLUUAC9 §3.2):
 *
 *  1.  Unseal  : CONTROL(0x8000) × 2
 *  1b. Verify  : CONTROL_STATUS SEC bits must be 00 or 01
 *  1c. Wake    : CONTROL(EXIT_HIBERNATE)
 *  2.  Mode    : CONTROL(SET_CFGUPDATE = 0x0013)
 *  3.  Poll    : FLAGS[4] (CFGUPD) = 1, timeout 3 s
 *  4.  Enable  : write 0x00 to reg 0x61 (BlockDataControl)
 *  5.  Class   : write 0x52 to reg 0x3E (DataFlashClass)   [FIX-1]
 *  6.  Block   : write 0x00 to reg 0x3F (DataFlashBlock = block 0)
 *  7.  Read    : 32 bytes from reg 0x40 → local block[]
 *  8.  Modify  : block[10-11] = cap (big-endian)
 *                block[12-13] = energy = cap * 3.7 (big-endian) [FIX-2]
 *  9.  Write   : 4 bytes individually to 0x4A, 0x4B, 0x4C, 0x4D
 *  10. Checksum: new_chk = 0xFF - (sum of local block[0..31] & 0xFF)
 *                *** Computed from local buffer — NO read from 0x60 ***
 *                *** Reading 0x60 resets chip state → NACK on write ***
 *                [FIX-3]
 *  11. Commit  : write new_chk to reg 0x60
 *  12. Reset   : CONTROL(SOFT_RESET = 0x0042), wait 500 ms
 *  13. Poll    : FLAGS[4] (CFGUPD) = 0
 *  14. Re-unseal, verify data flash, IT_ENABLE, Seal
 */
esp_err_t bq27441_reprogram_capacity(uint16_t capacity_mah)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    esp_err_t ret   = ESP_OK;
    uint16_t  flags = 0;
    uint8_t   buf[2];
    bool      did_write = false;

    /* [FIX-2] Design Energy = cap × 3.7 V */
    uint32_t  e32             = ((uint32_t)capacity_mah * 37u) / 10u;
    uint16_t  design_energy   = (e32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)e32;

    ESP_LOGI(TAG, "Starting capacity reprogramming: %u mAh / %u mWh",
             capacity_mah, design_energy);

    /* ---- 1. Unseal ---- */
    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Unseal #1 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Unseal #2 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ---- 1b. Verify security state ---- */
    {
        uint16_t cs = 0;
        ctrl_write(BQ27441_CTRL_STATUS);
        vTaskDelay(pdMS_TO_TICKS(5));
        cmd_read16(BQ27441_CMD_CONTROL, &cs);
        uint8_t sec = (uint8_t)((cs >> 14) & 0x03);
        ESP_LOGI(TAG, "CONTROL_STATUS=0x%04X  SEC=%u (%s)", cs, sec,
                 sec == 0x00 ? "FullAccess" :
                 sec == 0x01 ? "Unsealed" : "Sealed");
        if (sec == 0x03) {
            ESP_LOGE(TAG, "Device still SEALED — aborting");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto seal;
        }
    }

    /* ---- 1c. Exit hibernate ---- */
    ctrl_write(BQ27441_CTRL_EXIT_HIBERNATE);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 2. SET_CFGUPDATE ---- */
    ret = ctrl_write(BQ27441_CTRL_SET_CFGUPDATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SET_CFGUPDATE failed: %s", esp_err_to_name(ret));
        goto seal;
    }

    /* ---- 3. Poll CFGUPD flag, up to 3 s ---- */
    for (int i = 0; i < 60; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (cmd_read16(BQ27441_CMD_FLAGS, &flags) == ESP_OK &&
            (flags & BQ27441_FLAG_CFGUPD))
            break;
    }
    if (!(flags & BQ27441_FLAG_CFGUPD)) {
        ESP_LOGE(TAG, "CFGUPDATE timeout (FLAGS=0x%04X)", flags);
        ret = ESP_ERR_TIMEOUT;
        goto soft_reset;
    }
    ESP_LOGD(TAG, "CFGUPDATE active (FLAGS=0x%04X)", flags);

    /* ---- 4. Enable block data access ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_CTRL; buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BlockDataControl failed: %s", esp_err_to_name(ret));
        goto soft_reset;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 5. Select subclass 82 (State) [FIX-1] ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_CLASS;
    buf[1] = BQ27441_DF_CLASS_STATE;  /* 0x52 */
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DataFlashClass failed: %s", esp_err_to_name(ret));
        goto soft_reset;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 6. Select block 0 ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_OFFSET; buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DataFlashBlock failed: %s", esp_err_to_name(ret));
        goto soft_reset;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 7. Read 32-byte block into local buffer ---- */
    uint8_t block[32];
    {
        uint8_t reg = BQ27441_CMD_BLOCK_DATA;  /* 0x40 */
        ret = i2c_dev_support_write_read(s_dev, &reg, 1, block, 32, 200);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Block read failed: %s", esp_err_to_name(ret));
            goto soft_reset;
        }
    }
    ESP_LOGI(TAG, "Block[0-7]:  %02X %02X %02X %02X %02X %02X %02X %02X",
             block[0], block[1], block[2], block[3],
             block[4], block[5], block[6], block[7]);
    ESP_LOGI(TAG, "Block[8-15]: %02X %02X %02X %02X %02X %02X %02X %02X",
             block[8],  block[9],  block[10], block[11],
             block[12], block[13], block[14], block[15]);

    /* Parse current values (big-endian, MSB at lower offset) */
    uint16_t old_cap =
        ((uint16_t)block[BQ27441_DESIGN_CAP_OFFSET]     << 8) |
         (uint16_t)block[BQ27441_DESIGN_CAP_OFFSET + 1];
    uint16_t old_energy =
        ((uint16_t)block[BQ27441_DESIGN_ENERGY_OFFSET]     << 8) |
         (uint16_t)block[BQ27441_DESIGN_ENERGY_OFFSET + 1];
    ESP_LOGI(TAG, "Flash: DESIGN_CAPACITY=%u mAh  DESIGN_ENERGY=%u mWh",
             old_cap, old_energy);

    if (old_cap == capacity_mah && old_energy == design_energy) {
        ESP_LOGI(TAG, "Already %u mAh / %u mWh — skip write",
                 capacity_mah, design_energy);
        goto soft_reset;
    }

    /* ---- 8. Modify local buffer [FIX-2] ---- */
    block[BQ27441_DESIGN_CAP_OFFSET]     = (uint8_t)(capacity_mah >> 8);
    block[BQ27441_DESIGN_CAP_OFFSET + 1] = (uint8_t)(capacity_mah & 0xFF);
    block[BQ27441_DESIGN_ENERGY_OFFSET]     = (uint8_t)(design_energy >> 8);
    block[BQ27441_DESIGN_ENERGY_OFFSET + 1] = (uint8_t)(design_energy & 0xFF);

    /* ---- 9. Write entire modified block (single transaction) ---- */
    {
        uint8_t wb[33];
        wb[0] = BQ27441_CMD_BLOCK_DATA;  /* 0x40 — start register */
        memcpy(&wb[1], block, 32);
        ret = i2c_dev_support_write(s_dev, wb, 33, 200);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Block write failed: %s", esp_err_to_name(ret));
            goto soft_reset;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* ---- 10-11. Compute checksum from local buffer and commit [FIX-3] ----
     *
     * new_chk = 0xFF - (sum(block[0..31]) & 0xFF)
     *
     * Using the LOCAL modified block[] is correct because:
     *  - We loaded all 32 bytes from the chip (step 7).
     *  - We wrote exactly 4 bytes back to the chip (step 9).
     *  - The chip's internal buffer now matches our block[].
     *
     * CRITICAL: Do NOT read reg 0x60 between steps 7 and 11.
     * Reading 0x60 causes the BQ27441 to discard its loaded block
     * from internal RAM, making the subsequent write to 0x60 invalid
     * (chip returns NACK → i2c_master_transmit → ESP_ERR_INVALID_RESPONSE).
     */
    {
        uint8_t sum = 0;
        for (int i = 0; i < 32; i++)
            sum += block[i];
        uint8_t new_chk = (uint8_t)(0xFF - sum);
        ESP_LOGI(TAG, "Checksum: block_sum=0x%02X  new_chk=0x%02X", sum, new_chk);

        buf[0] = BQ27441_CMD_BLOCK_DATA_CHECK;  /* 0x60 */
        buf[1] = new_chk;
        ret = i2c_dev_support_write(s_dev, buf, 2, 50);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Checksum write failed: %s", esp_err_to_name(ret));
            goto soft_reset;
        }
        vTaskDelay(pdMS_TO_TICKS(200));  /* NVM write time per TRM */
    }

    ESP_LOGI(TAG, "Block committed: %u mAh / %u mWh  (was %u / %u)",
             capacity_mah, design_energy, old_cap, old_energy);
    did_write = true;

soft_reset:
    /* ---- 12. Exit CFGUPDATE via SOFT_RESET ---- */
    {
        esp_err_t r = ctrl_write(BQ27441_CTRL_SOFT_RESET);
        if (r != ESP_OK)
            ESP_LOGW(TAG, "SOFT_RESET failed: %s", esp_err_to_name(r));
    }
    vTaskDelay(pdMS_TO_TICKS(500));  /* NVM commit post-CFGUPDATE ≤ 300 ms */

    /* ---- 13. Poll CFGUPD clear ---- */
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (cmd_read16(BQ27441_CMD_FLAGS, &flags) == ESP_OK &&
            !(flags & BQ27441_FLAG_CFGUPD))
            break;
    }

    /* ---- 13b. Re-unseal (SOFT_RESET returns to Sealed) ---- */
    ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    vTaskDelay(pdMS_TO_TICKS(50));
    ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ---- 13c. Post-write verification [FIX-4] ---- */
    if (did_write) {
        uint8_t vb[32];
        uint8_t bv[2];

        bv[0] = BQ27441_CMD_BLOCK_DATA_CTRL;  bv[1] = 0x00;
        i2c_dev_support_write(s_dev, bv, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(5));

        bv[0] = BQ27441_CMD_BLOCK_DATA_CLASS;
        bv[1] = BQ27441_DF_CLASS_STATE;  /* 0x52 */
        i2c_dev_support_write(s_dev, bv, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(5));

        bv[0] = BQ27441_CMD_BLOCK_DATA_OFFSET;  bv[1] = 0x00;
        i2c_dev_support_write(s_dev, bv, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t reg_v = BQ27441_CMD_BLOCK_DATA;
        if (i2c_dev_support_write_read(s_dev, &reg_v, 1, vb, 32, 200) == ESP_OK) {
            uint16_t v_cap    = ((uint16_t)vb[BQ27441_DESIGN_CAP_OFFSET]     << 8) |
                                 (uint16_t)vb[BQ27441_DESIGN_CAP_OFFSET + 1];
            uint16_t v_energy = ((uint16_t)vb[BQ27441_DESIGN_ENERGY_OFFSET]     << 8) |
                                 (uint16_t)vb[BQ27441_DESIGN_ENERGY_OFFSET + 1];
            if (v_cap == capacity_mah && v_energy == design_energy) {
                ESP_LOGI(TAG, "Verified: cap=%u mAh  energy=%u mWh  [OK]",
                         v_cap, v_energy);
            } else {
                ESP_LOGW(TAG, "Mismatch: wrote cap=%u/energy=%u, "
                              "read back cap=%u/energy=%u",
                         capacity_mah, design_energy, v_cap, v_energy);
                ret = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    /* ---- 13d. IT_ENABLE (must be unsealed) ---- */
    ctrl_write(BQ27441_CTRL_IT_ENABLE);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Impedance Track enabled");

seal:
    /* ---- 14. Seal ---- */
    {
        esp_err_t r = ctrl_write(BQ27441_CTRL_SEAL);
        if (r != ESP_OK)
            ESP_LOGW(TAG, "Seal failed: %s", esp_err_to_name(r));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Do a full charge->discharge cycle to calibrate SoC");
    return ret;
}

/* ------------------------------------------------------------------ */
/* Read DESIGN_CAPACITY from NVM (subclass 82, offset 10)              */
/* ------------------------------------------------------------------ */

esp_err_t bq27441_read_design_capacity(uint16_t *capacity_mah)
{
    if (!s_dev || !capacity_mah) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    uint8_t   buf[2];
    uint8_t   block[32];

    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY); if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));
    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY); if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(200));

    buf[0] = BQ27441_CMD_BLOCK_DATA_CTRL;  buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(5));

    buf[0] = BQ27441_CMD_BLOCK_DATA_CLASS;
    buf[1] = BQ27441_DF_CLASS_STATE;  /* 0x52 [FIX-4] */
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(5));

    buf[0] = BQ27441_CMD_BLOCK_DATA_OFFSET;  buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(5));

    {
        uint8_t reg = BQ27441_CMD_BLOCK_DATA;
        ret = i2c_dev_support_write_read(s_dev, &reg, 1, block, 32, 200);
        if (ret != ESP_OK) goto done;
    }

    *capacity_mah = ((uint16_t)block[BQ27441_DESIGN_CAP_OFFSET]     << 8) |
                     (uint16_t)block[BQ27441_DESIGN_CAP_OFFSET + 1];
    ESP_LOGD(TAG, "DESIGN_CAPACITY from flash: %u mAh", *capacity_mah);

done:
    ctrl_write(BQ27441_CTRL_SEAL);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ret;
}
