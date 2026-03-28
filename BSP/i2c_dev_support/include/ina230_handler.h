/**
 * @file ina230_handler.h
 * @brief INA230AIRGTR Power Monitor I2C Driver
 *
 * Hardware: I2C addr 0x40 (A0=GND, A1=GND), shared M_I2C0 bus
 * Shunt resistor R65 = 10mΩ on +4V2_VSYS rail
 * Calibrated for: max_current=6.55A, current_lsb=0.2mA/bit
 */

#ifndef INA230_HANDLER_H
#define INA230_HANDLER_H

#include "esp_err.h"
#include "i2c_dev_support.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2C address (A0=GND, A1=GND) */
#define INA230_I2C_ADDR     0x40
#define INA230_I2C_FREQ_HZ  400000

/* Register addresses */
#define INA230_REG_CONFIG       0x00
#define INA230_REG_SHUNT_V      0x01   /* Shunt voltage (LSB = 2.5μV) */
#define INA230_REG_BUS_V        0x02   /* Bus voltage (LSB = 1.25mV) */
#define INA230_REG_POWER        0x03   /* Power (LSB = 25 × current_lsb) */
#define INA230_REG_CURRENT      0x04   /* Current (signed, LSB = current_lsb) */
#define INA230_REG_CALIBRATION  0x05
#define INA230_REG_MASK_EN      0x06
#define INA230_REG_ALERT_LIMIT  0x07
#define INA230_REG_MANUFACTURER 0xFE
#define INA230_REG_DIE_ID       0xFF

/**
 * @brief INA230 power status snapshot
 */
typedef struct {
    uint16_t bus_voltage_mv;    /* VSYS rail voltage in mV */
    int16_t  current_ma;        /* Current in mA (positive = load current) */
    int32_t  power_mw;          /* Power in mW */
} ina230_status_t;

/**
 * @brief Initialize INA230 with calibration for R=10mΩ shunt.
 *        current_lsb = 0.2mA/bit, calibration = 2560, range: 0–6.55A.
 *        Requires i2c_dev_support_init() to be called first.
 * @return ESP_OK on success
 */
esp_err_t ina230_init(void);

/**
 * @brief Read VSYS bus voltage.
 * @param voltage_mv Output voltage in mV
 * @return ESP_OK on success
 */
esp_err_t ina230_read_bus_voltage_mv(uint16_t *voltage_mv);

/**
 * @brief Read system load current.
 * @param current_ma Output current in mA (signed)
 * @return ESP_OK on success
 */
esp_err_t ina230_read_current_ma(int16_t *current_ma);

/**
 * @brief Read all INA230 measurements at once.
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t ina230_read_status(ina230_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* INA230_HANDLER_H */
