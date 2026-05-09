/**
 * @file bq27441_handler.h
 * @brief BQ27441DRZR-G1B Battery Fuel Gauge I2C Driver
 *
 * Hardware: I2C addr 0x55 (fixed), shared M_I2C0 bus
 * Shunt resistor R72 = 10mΩ at battery terminals (SRN/SRP pins)
 */

#ifndef BQ27441_HANDLER_H
#define BQ27441_HANDLER_H

#include "esp_err.h"
#include "i2c_dev_support.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2C address (fixed for BQ27441) */
#define BQ27441_I2C_ADDR    0x55
#define BQ27441_I2C_FREQ_HZ 100000   /* BQ27441 max I2C speed = 400kHz, use 100kHz for reliability */

/* Standard command register addresses */
#define BQ27441_CMD_CONTROL     0x00   /* Control() — write sub-command */
#define BQ27441_CMD_TEMPERATURE 0x02   /* Temperature (0.1K units) */
#define BQ27441_CMD_VOLTAGE     0x04   /* Voltage (mV) */
#define BQ27441_CMD_FLAGS       0x06   /* Status flags */
#define BQ27441_CMD_NOM_CAP     0x08   /* Nominal Available Capacity (mAh) */
#define BQ27441_CMD_FULL_CAP    0x0A   /* Full Available Capacity (mAh) */
#define BQ27441_CMD_REM_CAP     0x0C   /* Remaining Capacity (mAh) */
#define BQ27441_CMD_AVG_POWER   0x18   /* Average Power (mW, signed) */
#define BQ27441_CMD_SOC         0x1C   /* State of Charge (%) */
#define BQ27441_CMD_INT_TEMP    0x1E   /* Internal temperature (0.1K) */
#define BQ27441_CMD_AVG_CURRENT 0x10   /* Average Current (mA, signed) */

/* Control sub-commands */
#define BQ27441_CTRL_STATUS        0x0000
#define BQ27441_CTRL_DEVICE_TYPE   0x0001
#define BQ27441_CTRL_FW_VERSION    0x0002
#define BQ27441_CTRL_UNSEAL_KEY    0x8000  /* Default unseal key (sent twice: 0x8000 then 0x8000) */
#define BQ27441_CTRL_SEAL          0x0020  /* Seal data flash (lock configuration) */
#define BQ27441_CTRL_SET_CFGUPDATE 0x0013  /* Enter CONFIG UPDATE mode (required before block data access) */
#define BQ27441_CTRL_SOFT_RESET      0x0042  /* Gauge reset — exits CFGUPDATE but RELOADS Data Memory from OTP */
#define BQ27441_CTRL_EXIT_CFGUPDATE  0x0043  /* Exits CFGUPDATE mode only — PRESERVES RAM-based Data Memory, NO SoC resim */
#define BQ27441_CTRL_EXIT_RESIM      0x0044  /* Exits CFGUPDATE + resimulates SoC from current voltage — use this */
#define BQ27441_CTRL_EXIT_HIBERNATE  0x0011  /* Wake from hibernate — resumes current measurements */
#define BQ27441_CTRL_IT_ENABLE       0x0021  /* Enable Impedance Track algorithm (activates SoC/current) */

/* Extended command for reading Design Capacity from Data Memory (Table 3, §9.5.3) */
#define BQ27441_CMD_DESIGN_CAP       0x3C    /* DesignCapacity() — readable in sealed+unsealed modes (mAh) */

/* Data Flash Block Interface - for accessing/modifying design capacity */
#define BQ27441_CMD_BLOCK_DATA_CTRL  0x61  /* Block Data Control status */
#define BQ27441_CMD_BLOCK_DATA_CLASS 0x3E  /* Block Data Class ID */
#define BQ27441_CMD_BLOCK_DATA_OFFSET 0x3F /* Block Data Offset (within class) */
#define BQ27441_CMD_BLOCK_DATA       0x40  /* Block Data Array (32 bytes) */
#define BQ27441_CMD_BLOCK_DATA_CHECK 0x60  /* Block Data Checksum */

/* Data Flash class IDs + offsets */
/* Class 82 (0x52) = "State" subclass per TRM SLUUAC9 — contains Design Capacity & Design Energy */
#define BQ27441_DF_CLASS_STATE      0x52   /* Subclass 82 = State (Design Capacity, Design Energy, etc.) */
#define BQ27441_DF_CLASS_POWER      BQ27441_DF_CLASS_STATE  /* Alias kept for backward compat — same class */
#define BQ27441_DESIGN_CAP_OFFSET   0x0A   /* Design Capacity at offset 10-11 within subclass 82 block 0 (big-endian) */
#define BQ27441_DESIGN_ENERGY_OFFSET 0x0C  /* Design Energy   at offset 12-13 within subclass 82 block 0 (big-endian) */

/* Flags bit definitions */
#define BQ27441_FLAG_CFGUPD     (1 << 4)    /* CONFIG UPDATE mode active (block data access enabled) */
#define BQ27441_FLAG_DSG        (1 << 0)    /* Discharging */
#define BQ27441_FLAG_SOCF       (1 << 1)    /* State of Charge Final (critical low) */
#define BQ27441_FLAG_SOC1       (1 << 2)    /* SoC threshold 1 */
#define BQ27441_FLAG_BAT_DET    (1 << 3)    /* Battery detected */
#define BQ27441_FLAG_TCA        (1 << 9)    /* Terminate Charge Alarm */
#define BQ27441_FLAG_OT         (1 << 11)   /* Over Temperature */
#define BQ27441_FLAG_FC         (1 << 12)   /* Full Charge */
#define BQ27441_FLAG_UT         (1 << 15)   /* Under Temperature */

/**
 * @brief BQ27441 measurement snapshot
 */
typedef struct {
    uint16_t voltage_mv;      /* Battery terminal voltage (mV) */
    uint8_t  soc_pct;         /* State of Charge (0–100 %) */
    int16_t  avg_current_ma;  /* Average current (mA; negative = discharging) */
    int16_t  avg_power_mw;    /* Average power (mW; negative = discharging) */
    uint16_t flags;           /* Raw FLAGS register */
    bool     fully_charged;   /* FC flag */
    bool     critical_low;    /* SOCF flag (battery critically low) */
    bool     battery_present; /* BAT_DET flag */
} bq27441_status_t;

/**
 * @brief Initialize BQ27441 and verify device presence.
 *        Requires i2c_dev_support_init() to be called first.
 * @return ESP_OK on success
 */
esp_err_t bq27441_init(void);

/**
 * @brief Read battery terminal voltage.
 * @param voltage_mv Output voltage in mV
 * @return ESP_OK on success
 */
esp_err_t bq27441_read_voltage_mv(uint16_t *voltage_mv);

/**
 * @brief Read State of Charge.
 * @param soc_pct Output SoC percentage (0–100)
 * @return ESP_OK on success
 */
esp_err_t bq27441_read_soc_pct(uint8_t *soc_pct);

/**
 * @brief Read FLAGS register.
 * @param flags Output raw flags (use BQ27441_FLAG_* bitmasks)
 * @return ESP_OK on success
 */
esp_err_t bq27441_read_flags(uint16_t *flags);

/**
 * @brief Read average current.
 * @param current_ma Output current in mA (signed; negative = discharging)
 * @return ESP_OK on success
 */
esp_err_t bq27441_read_avg_current_ma(int16_t *current_ma);

/**
 * @brief Read all measurements at once.
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t bq27441_read_status(bq27441_status_t *status);

/**
 * @brief Reprogram battery DESIGN_CAPACITY via data flash unsealing.
 *        Requires battery to be present and fully discharged below 2.5V for reliable operation.
 *        This function will:
 *        1. Unseal the data flash using security codes
 *        2. Write new DESIGN_CAPACITY to data flash class 82 (Power)
 *        3. Recalculate and write checksum
 *        4. Seal the data flash to protect configuration
 * @param capacity_mah Target design capacity in mAh (e.g., 3000 for 3000mAh battery)
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t bq27441_reprogram_capacity(uint16_t capacity_mah);

/**
 * @brief Read current DESIGN_CAPACITY from data flash class 82 (Power).
 * This is the battery's configured capacity (not remaining charge, not available capacity).
 * Useful for checking if a reprogram is needed before calling bq27441_reprogram_capacity().
 * @param capacity_mah Output buffer for design capacity in mAh
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t bq27441_read_design_capacity(uint16_t *capacity_mah);

/**
 * @brief Estimate State of Charge from Open Circuit Voltage (OCV).
 *        Uses an OCV-to-SoC lookup table for LiCoO2 chemistry with linear interpolation.
 *        This is a fallback when BQ27441's IT algorithm fails (SoC stuck at 0%).
 *        Gives a rough estimate independent of coulomb counting history.
 * @param voltage_mv Battery voltage in mV
 * @return Estimated SoC percentage (0–100)
 */
uint8_t bq27441_estimate_soc_from_ocv(uint16_t voltage_mv);

#ifdef __cplusplus
}
#endif

#endif /* BQ27441_HANDLER_H */
