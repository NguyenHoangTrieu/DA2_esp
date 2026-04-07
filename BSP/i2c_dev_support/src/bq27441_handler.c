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
/*  Data Flash Access for Capacity Reprogramming                     */
/* ================================================================== */

/**
 * @brief Read a byte from the Data Flash Block Data buffer (0x40).
 */
static esp_err_t block_read_byte(uint8_t offset, uint8_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;

    /* First set Block Data Class ID */
    uint8_t buf[2] = {BQ27441_CMD_BLOCK_DATA_CLASS, BQ27441_DF_CLASS_POWER};
    esp_err_t ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Set Block Data Offset */
    buf[0] = BQ27441_CMD_BLOCK_DATA_OFFSET;
    buf[1] = offset;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Read the byte from Block Data register (0x40) */
    uint8_t reg_addr = BQ27441_CMD_BLOCK_DATA;
    return i2c_dev_support_write_read(s_dev, &reg_addr, 1, value, 1, 50);
}

/**
 * @brief Write a byte to the Data Flash Block Data buffer (0x40).
 */
static esp_err_t block_write_byte(uint8_t offset, uint8_t value)
{
    /* First set Block Data Class ID */
    uint8_t buf[2] = {BQ27441_CMD_BLOCK_DATA_CLASS, BQ27441_DF_CLASS_POWER};
    esp_err_t ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) return ret;

    /* Set Block Data Offset */
    buf[0] = BQ27441_CMD_BLOCK_DATA_OFFSET;
    buf[1] = offset;
    ret = i2c_dev_support_write(s_dev, buf, 2, 50);
    if (ret != ESP_OK) return ret;

    /* Write the byte to Block Data position */
    buf[0] = BQ27441_CMD_BLOCK_DATA;
    buf[1] = value;
    return i2c_dev_support_write(s_dev, buf, 2, 50);
}

/**
 * @brief Read the Block Data Checksum byte.
 */
static esp_err_t block_read_checksum(uint8_t *checksum)
{
    if (!checksum) return ESP_ERR_INVALID_ARG;
    uint8_t reg_addr = BQ27441_CMD_BLOCK_DATA_CHECK;
    return i2c_dev_support_write_read(s_dev, &reg_addr, 1, checksum, 1, 50);
}

/**
 * @brief Write new checksum to Block Data Checksum register.
 */
static esp_err_t block_write_checksum(uint8_t checksum)
{
    uint8_t buf[2] = {BQ27441_CMD_BLOCK_DATA_CHECK, checksum};
    return i2c_dev_support_write(s_dev, buf, 2, 50);
}

/**
 * @brief Reprogram DESIGN_CAPACITY (battery capacity in mAh).
 */
esp_err_t bq27441_reprogram_capacity(uint16_t capacity_mah)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Starting capacity reprogramming: %u mAh", capacity_mah);

    /* Step 1: Unlock the data flash by sending unseal codes */
    ESP_LOGD(TAG, "Unsealing data flash...");
    esp_err_t ret = ctrl_write(BQ27441_CTRL_UNSEAL_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write unseal code (low): %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = ctrl_write(BQ27441_CTRL_UNSEAL_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write unseal code (high): %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 2: Read current design capacity bytes via Block Data interface */
    ESP_LOGD(TAG, "Reading current data flash values...");
    uint8_t cap_low = 0, cap_high = 0;
    ret = block_read_byte(BQ27441_DESIGN_CAP_OFFSET, &cap_low);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read design cap low byte: %s", esp_err_to_name(ret));
        cap_low = 0;
    }

    ret = block_read_byte(BQ27441_DESIGN_CAP_OFFSET + 1, &cap_high);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read design cap high byte: %s", esp_err_to_name(ret));
        cap_high = 0;
    }
    uint16_t old_cap = (uint16_t)cap_low | ((uint16_t)cap_high << 8);
    ESP_LOGI(TAG, "Current DESIGN_CAPACITY: %u mAh", old_cap);

    /* Step 3: Write new capacity values */
    ESP_LOGD(TAG, "Writing new capacity bytes...");
    cap_low = (uint8_t)(capacity_mah & 0xFF);
    cap_high = (uint8_t)((capacity_mah >> 8) & 0xFF);

    ret = block_write_byte(BQ27441_DESIGN_CAP_OFFSET, cap_low);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write design cap low byte: %s", esp_err_to_name(ret));
        goto seal_and_exit;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    ret = block_write_byte(BQ27441_DESIGN_CAP_OFFSET + 1, cap_high);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write design cap high byte: %s", esp_err_to_name(ret));
        goto seal_and_exit;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  /* Wait longer after writes before reading back */

    ESP_LOGI(TAG, "Design capacity write complete (0x%02X 0x%02X → %u mAh)", cap_low, cap_high, capacity_mah);

    /* Step 4: Recalculate and write checksum
       BQ27441 checksum = 0xFF - (sum of bytes 0-30, excluding byte 31 which is the checksum itself) */
    uint8_t block_sum = 0;
    for (int i = 0; i < 31; i++) {  /* Only sum bytes 0-30, NOT byte 31 (checksum byte) */
        uint8_t byte_val = 0;
        esp_err_t read_ret = block_read_byte(i, &byte_val);
        if (read_ret == ESP_OK) {
            block_sum += byte_val;
            ESP_LOGD(TAG, "  Byte[%d]=0x%02X, sum=0x%02X", i, byte_val, block_sum);
        }
    }
    uint8_t new_checksum = 0xFF - block_sum;
    
    ESP_LOGI(TAG, "Checksum calculation: sum(0-30)=0x%02X, new_checksum=0xFF-0x%02X=0x%02X", 
             block_sum, block_sum, new_checksum);
    ret = block_write_checksum(new_checksum);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write checksum: %s", esp_err_to_name(ret));
        goto seal_and_exit;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Capacity reprogramming completed: %u mAh (was %u mAh)", capacity_mah, old_cap);

seal_and_exit:
    /* Step 5: Seal the data flash to protect configuration */
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGD(TAG, "Sealing data flash...");
    esp_err_t seal_ret = ctrl_write(BQ27441_CTRL_SEAL);
    if (seal_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to seal data flash: %s", esp_err_to_name(seal_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* Verify the new capacity was written */
    uint16_t verify_cap = 0;
    if (cmd_read16(BQ27441_CMD_NOM_CAP, &verify_cap) == ESP_OK) {
        ESP_LOGI(TAG, "Verified DESIGN_CAPACITY: %u mAh", verify_cap);
    }

    return ret;
}
