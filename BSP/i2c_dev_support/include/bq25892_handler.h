/**
 * @file bq25892_handler.h
 * @brief BQ25892RTWR Battery Charger I2C Driver
 *
 * Hardware: I2C addr 0x6B, shared M_I2C0 bus (GPIO1=SCL, GPIO2=SDA)
 * ILIM resistor R66 = 120Ω → hardware ILIM ≈ 2.96A
 * Charge voltage target: 4.096V (≈4.1V upper threshold)
 */

#ifndef BQ25892_HANDLER_H
#define BQ25892_HANDLER_H

#include "esp_err.h"
#include "i2c_dev_support.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2C address */
#define BQ25892_I2C_ADDR    0x6B
#define BQ25892_I2C_FREQ_HZ 400000

/* Register map */
#define BQ25892_REG00   0x00   /* Input Source Control (EN_HIZ, EN_ILIM, IINLIM) */
#define BQ25892_REG02   0x02   /* ADC Control (CONV_START, CONV_RATE) */
#define BQ25892_REG03   0x03   /* Charge Control (OTG_CONFIG, CHG_CONFIG, SYS_MIN) */
#define BQ25892_REG06   0x06   /* Charge Voltage Limit (VREG[5:0], BATLOWV, VRECHG) */
#define BQ25892_REG09   0x09   /* Fault / BATFET_DIS Register */
#define BQ25892_REG0B   0x0B   /* System Status (VBUS_STAT, CHRG_STAT, PG_STAT) */
#define BQ25892_REG0E   0x0E   /* Battery Voltage ADC (THERM_STAT, BATV[6:0]) */
#define BQ25892_REG14   0x14   /* Device ID / Rev register */

/* REG03 bits */
#define BQ25892_CHG_CONFIG_BIT  (1 << 4)   /* 1 = enable charging */
#define BQ25892_OTG_CONFIG_BIT  (1 << 5)   /* 1 = enable OTG boost */

/* REG09 bits */
#define BQ25892_BATFET_DIS_BIT  (1 << 5)   /* 1 = disconnect battery FET (battery isolated) */

/* REG0B charge status field (bits [4:3]) */
#define BQ25892_CHRG_STAT_MASK      0x18
#define BQ25892_CHRG_STAT_NOT_CHG   0x00
#define BQ25892_CHRG_STAT_PRE_CHG   0x08
#define BQ25892_CHRG_STAT_FAST_CHG  0x10
#define BQ25892_CHRG_STAT_DONE      0x18

/* Target charge voltage (4.096V ≈ 4.1V) */
#define BQ25892_VREG_4096MV  16  /* VREG[5:0]: voltage = 3840 + VREG×16 mV */
#define BQ25892_VREG_4112MV  17  /* 4112 mV */

/**
 * @brief BQ25892 charge status
 */
typedef enum {
    BQ25892_STATUS_NOT_CHARGING = 0,
    BQ25892_STATUS_PRE_CHARGING,
    BQ25892_STATUS_FAST_CHARGING,
    BQ25892_STATUS_CHARGE_DONE,
} bq25892_chrg_status_t;

/**
 * @brief BQ25892 runtime status snapshot
 */
typedef struct {
    bq25892_chrg_status_t chrg_status;  /* Charge phase */
    bool     power_good;                 /* True if valid VBUS present */
    uint16_t vbat_mv;                    /* Battery voltage from ADC (mV) */
    bool     charge_enabled;             /* Current CHG_CONFIG state */
    uint8_t  fault_reg;                  /* REG09 raw fault register */
} bq25892_status_t;

/**
 * @brief Initialize BQ25892 and configure for new board settings.
 *        Sets IINLIM=3A, VREG=4096mV (4.1V), enables charging.
 *        Requires i2c_dev_support_init() to be called first.
 * @return ESP_OK on success
 */
esp_err_t bq25892_init(void);

/**
 * @brief Enable or disable battery charging.
 * @param enable true to charge, false to stop charging
 * @return ESP_OK on success
 */
esp_err_t bq25892_set_charge_enable(bool enable);

/**
 * @brief Set charge termination voltage.
 * @param vreg_mv Target voltage in mV (3840–4608). Rounded to 16 mV steps.
 * @return ESP_OK on success
 */
esp_err_t bq25892_set_charge_voltage_mv(uint16_t vreg_mv);

/**
 * @brief Enable or disable OTG boost (battery → VBUS).
 * @param enable true to enable OTG
 * @return ESP_OK on success
 */
esp_err_t bq25892_set_otg(bool enable);

/**
 * @brief Trigger a single battery voltage ADC conversion,
 *        then read back the battery voltage.
 * @param vbat_mv Output: battery voltage in mV
 * @return ESP_OK on success
 */
esp_err_t bq25892_read_batv_mv(uint16_t *vbat_mv);

/**
 * @brief Enable or disable the battery FET (BATFET_DIS bit in REG09).
 *        When disabled, battery is disconnected from the system (VSYS falls to VBUS only).
 *        When enabled (default), battery supplies power normally.
 * @param disable true to disconnect battery, false to reconnect
 * @return ESP_OK on success
 */
esp_err_t bq25892_set_batfet_disable(bool disable);

/**
 * @brief Read current charge status and fault flags.
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t bq25892_read_status(bq25892_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* BQ25892_HANDLER_H */
