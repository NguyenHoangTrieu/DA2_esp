/**
 * @file bq27441_handler.c
 * @brief BQ27441DRZR-G1B Battery Fuel Gauge I2C Driver Implementation
 */

#include "bq27441_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /* Read nominal capacity (DESIGN_CAPACITY set during manufacturing or by unsealing) */
    uint16_t nom_cap = 0;
    cmd_read16(BQ27441_CMD_NOM_CAP, &nom_cap);
    ESP_LOGI(TAG, "Nominal Capacity: %u mAh", nom_cap);

    if (nom_cap != 3000) {
        ESP_LOGW(TAG, "Nominal capacity is %u mAh, expected 3000 mAh. SoC may be inaccurate.", nom_cap);
        ESP_LOGW(TAG, "To reprogram: battery must be discharged, then use unsealing procedure");
    }

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

/* ================================================================== */
/*  Data Flash — Capacity Reprogramming                              */
/* ================================================================== */

/**
 * @brief Reprogram DESIGN_CAPACITY in data memory class 82 (Power).
 *
 * Correct BQ27441-G1 data flash write sequence:
 *  1.  Unseal: CONTROL(0x8000) twice  [default key 0x80008000]
 *  2.  Enter CFGUPDATE: CONTROL(0x0013)
 *  3.  Poll FLAGS[4] (CFGUPD) until = 1  (up to 2.5 s)
 *  4.  BlockDataControl(0x61) = 0x00    [enable block data access]
 *  5.  DataFlashClass(0x3E)   = 0x52    [class 82 = Power]
 *  6.  DataFlashBlock(0x3F)   = 0x00    [block 0: bytes 0-31]
 *  7.  Read 32 bytes from BlockData(0x40)
 *  8.  Modify bytes 0-1: DESIGN_CAPACITY big-endian (MSB first)
 *  9.  Write modified bytes to 0x40 and 0x41
 *  10. Checksum = 0xFF - sum(all 32 modified bytes)
 *  11. Write checksum to BlockDataChecksum(0x60)
 *  12. SOFT_RESET: CONTROL(0x0042)  [commit + exit CFGUPDATE]
 *  13. Poll FLAGS[4] until = 0
 *  14. Seal: CONTROL(0x0020)
 *
 * Note: CMD_NOM_CAP (0x08) returns NominalAvailableCapacity (remaining charge),
 *       NOT DESIGN_CAPACITY. Verify via a second block-data read if needed.
 *       SoC will be inaccurate until a full charge→discharge conditioning cycle.
 */
esp_err_t bq27441_reprogram_capacity(uint16_t capacity_mah)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    uint16_t  flags = 0;
    uint8_t   buf[2];

    ESP_LOGI(TAG, "Starting capacity reprogramming: %u mAh", capacity_mah);

    /* ---- 1. Unseal with default key: write 0x8000 twice ---- */
    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Unseal #1 failed: %s", esp_err_to_name(ret)); return ret; }
    vTaskDelay(pdMS_TO_TICKS(50));   /* Allow IC to process first key */

    ret = ctrl_write(BQ27441_CTRL_UNSEAL_KEY);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Unseal #2 failed: %s", esp_err_to_name(ret)); return ret; }
    vTaskDelay(pdMS_TO_TICKS(200));  /* Allow IC to process security state transition */

    /* ---- 1b. Verify unseal via CONTROL_STATUS — bits[14:13]: 01=Unsealed, 11=Sealed ---- */
    {
        uint16_t ctrl_status = 0;
        ctrl_write(BQ27441_CTRL_STATUS);   /* 0x0000: request CONTROL_STATUS */
        vTaskDelay(pdMS_TO_TICKS(5));
        cmd_read16(BQ27441_CMD_CONTROL, &ctrl_status);
        uint8_t sec = (uint8_t)((ctrl_status >> 13) & 0x03);
        ESP_LOGI(TAG, "CONTROL_STATUS=0x%04X  SECURITY=%u (%s)",
                 ctrl_status, sec,
                 sec == 0x01 ? "Unsealed" : sec == 0x00 ? "FullAccess" : "Sealed");
        if (sec == 0x03) {
            ESP_LOGE(TAG, "Device still SEALED after unseal attempt — reprogram skipped");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto seal;  /* skip CFGUPDATE entirely, go straight to seal (no-op) */
        }
    }

    /* ---- 1c. EXIT_HIBERNATE (requires UNSEALED) — BQ27441 may be in hibernate ---- */
    /* If the device entered hibernate before this call, SET_CFGUPDATE will be silently
     * ignored even when unsealed. Wake the IC first, then wait for it to settle. */
    ctrl_write(BQ27441_CTRL_EXIT_HIBERNATE);
    vTaskDelay(pdMS_TO_TICKS(50));
    ctrl_write(BQ27441_CTRL_IT_ENABLE);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* ---- 2. Enter CFGUPDATE mode ---- */
    ret = ctrl_write(BQ27441_CTRL_SET_CFGUPDATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SET_CFGUPDATE failed: %s", esp_err_to_name(ret));
        goto seal;
    }

    /* ---- 3. Poll FLAGS[4] (CFGUPD) — up to 1 s ---- */
    flags = 0;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (cmd_read16(BQ27441_CMD_FLAGS, &flags) == ESP_OK && (flags & BQ27441_FLAG_CFGUPD)) break;
    }
    if (!(flags & BQ27441_FLAG_CFGUPD)) {
        ESP_LOGE(TAG, "CFGUPDATE mode timeout (FLAGS=0x%04X)", flags);
        ret = ESP_ERR_TIMEOUT;
        goto soft_reset;
    }
    ESP_LOGD(TAG, "CFGUPDATE active (FLAGS=0x%04X)", flags);

    /* ---- 4. Enable block data access ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_CTRL;  buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BlockDataControl failed: %s", esp_err_to_name(ret)); goto soft_reset; }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 5. Select class 82 (Power) ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_CLASS; buf[1] = BQ27441_DF_CLASS_POWER;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "DataFlashClass failed: %s", esp_err_to_name(ret)); goto soft_reset; }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 6. Select block 0 (bytes 0-31; DESIGN_CAPACITY at offset 0) ---- */
    buf[0] = BQ27441_CMD_BLOCK_DATA_OFFSET; buf[1] = 0x00;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "DataFlashBlock failed: %s", esp_err_to_name(ret)); goto soft_reset; }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ---- 7. Read all 32 bytes ---- */
    uint8_t block[32];
    {
        uint8_t reg = BQ27441_CMD_BLOCK_DATA;  /* 0x40 */
        ret = i2c_dev_support_write_read(s_dev, &reg, 1, block, 32, 200);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Block read failed: %s", esp_err_to_name(ret));
            goto soft_reset;
        }
    }
    ESP_LOGI(TAG, "Block[0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
             block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7]);
    ESP_LOGI(TAG, "Block[8-15]: %02X %02X %02X %02X %02X %02X %02X %02X",
             block[8], block[9], block[10], block[11], block[12], block[13], block[14], block[15]);

    /* ---- 8. Parse current DESIGN_CAPACITY (big-endian: MSB at offset 0) ---- */
    uint16_t old_cap = ((uint16_t)block[BQ27441_DESIGN_CAP_OFFSET] << 8)
                     |  (uint16_t)block[BQ27441_DESIGN_CAP_OFFSET + 1];
    ESP_LOGI(TAG, "Current DESIGN_CAPACITY: %u mAh (bytes: 0x%02X 0x%02X)",
             old_cap, block[BQ27441_DESIGN_CAP_OFFSET], block[BQ27441_DESIGN_CAP_OFFSET + 1]);

    if (old_cap == capacity_mah) {
        ESP_LOGI(TAG, "Already %u mAh — no change needed", capacity_mah);
        ret = ESP_OK;
        goto soft_reset;
    }

    /* ---- 8 (cont). Modify bytes — big-endian, MSB at lower offset ---- */
    block[BQ27441_DESIGN_CAP_OFFSET]     = (uint8_t)((capacity_mah >> 8) & 0xFF);  /* MSB */
    block[BQ27441_DESIGN_CAP_OFFSET + 1] = (uint8_t)(capacity_mah & 0xFF);          /* LSB */

    /* ---- 9. Write ALL 32 modified bytes sequentially to 0x40..0x5F ---- */
    /* BQ27441 requires the full 32-byte block to be written so the chip's
     * internal checksum accumulator covers all bytes, not just the changed ones. */
    {
        /* Build a 33-byte buffer: register 0x40 followed by 32 data bytes */
        uint8_t wbuf[33];
        wbuf[0] = BQ27441_CMD_BLOCK_DATA;  /* 0x40 */
        for (int i = 0; i < 32; i++) wbuf[i + 1] = block[i];
        ret = i2c_dev_support_write(s_dev, wbuf, 33, 200);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "Block write failed: %s", esp_err_to_name(ret)); goto soft_reset; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* ---- 10-11. Checksum = 0xFF - (sum of all 32 modified bytes), then write ---- */
    {
        uint8_t sum = 0;
        for (int i = 0; i < 32; i++) sum += block[i];
        uint8_t new_chk = (uint8_t)(0xFF - sum);
        ESP_LOGI(TAG, "Checksum: block_sum=0x%02X → new_checksum=0x%02X", sum, new_chk);

        /* Write checksum — this commits the block data to flash */
        buf[0] = BQ27441_CMD_BLOCK_DATA_CHECK;  /* 0x60 */
        buf[1] = new_chk;
        ret = i2c_dev_support_write(s_dev, buf, 2, 50);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Write checksum failed: %s", esp_err_to_name(ret));
            goto soft_reset;
        }
        vTaskDelay(pdMS_TO_TICKS(150));  /* Wait for flash write to complete */
    }
    ESP_LOGI(TAG, "Capacity write complete: %u mAh (was %u mAh)", capacity_mah, old_cap);

soft_reset:
    /* ---- 12. Exit CFGUPDATE via SOFT_RESET ---- */
    {
        esp_err_t r = ctrl_write(BQ27441_CTRL_SOFT_RESET);
        if (r != ESP_OK) ESP_LOGW(TAG, "SOFT_RESET failed: %s", esp_err_to_name(r));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ---- 13. Wait for FLAGS[4] (CFGUPD) to clear ---- */
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (cmd_read16(BQ27441_CMD_FLAGS, &flags) == ESP_OK && !(flags & BQ27441_FLAG_CFGUPD)) break;
    }

seal:
    /* ---- 14. Seal ---- */
    {
        esp_err_t r = ctrl_write(BQ27441_CTRL_SEAL);
        if (r != ESP_OK) ESP_LOGW(TAG, "Seal failed: %s", esp_err_to_name(r));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Post-program status: CMD_NOM_CAP is remaining capacity, NOT design capacity */
    {
        uint16_t nom = 0;
        cmd_read16(BQ27441_CMD_NOM_CAP, &nom);
        ESP_LOGI(TAG, "NomCap after reprogram: %u mAh (this is remaining charge, not design cap)", nom);
        ESP_LOGI(TAG, "Do a full charge→discharge cycle to calibrate SoC against new 3000 mAh design");
    }

    return ret;
}
