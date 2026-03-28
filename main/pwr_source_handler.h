/**
 * @file pwr_source_handler.h
 * @brief Power Management Coordinator — BQ25892 + INA230 + BQ27441
 *
 * New board replaces TCA6424A power-rail control with dedicated ICs:
 *   - BQ25892RTWR  (0x6B): Battery charger, VREG=4.1V, ILIM=3A
 *   - INA230AIRGTR (0x40): Current/voltage monitor on VSYS (R65=10mΩ)
 *   - BQ27441DRZR  (0x55): Fuel gauge at battery terminals (R72=10mΩ)
 *
 * BC_IO GPIO numbers (verify from schematic netlist — set to GPIO_NUM_NC until confirmed).
 */

#ifndef PWR_SOURCE_HANDLER_H
#define PWR_SOURCE_HANDLER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Battery voltage thresholds */
#define PWR_BATT_UPPER_THRESHOLD_MV   4100   /* Stop charging above this voltage */
#define PWR_BATT_LOWER_THRESHOLD_MV   3500   /* Low battery alert below this voltage */

/*
 * BC_IO discrete GPIO pins — TODO: verify from full schematic netlist.
 * Use GPIO_NUM_NC (-1) for unconfirmed pins; driver skips GPIO ops for NC pins.
 */
#define BC_CE_GPIO_NUM    GPIO_NUM_NC   /* Charge Enable (active-low output) */
#define BC_OTG_GPIO_NUM   GPIO_NUM_NC   /* OTG Enable (output) */
#define BC_INT_GPIO_NUM   GPIO_NUM_NC   /* Charger Interrupt (input, falling edge) */
#define BC_STAT_GPIO_NUM  GPIO_NUM_NC   /* Charge Status LED driver (input) */
#define BC_PG_GPIO_NUM    GPIO_NUM_NC   /* Power Good (input, active-low) */
#define BC_PSEL_GPIO_NUM  GPIO_NUM_NC   /* Power Source Select (output) */

/**
 * @brief Combined power status snapshot (all 3 ICs + GPIO).
 */
typedef struct {
    /* From BQ27441 (fuel gauge at battery terminals) */
    uint16_t vbat_mv;           /* Battery terminal voltage (mV) */
    uint8_t  soc_pct;           /* State of charge (0–100 %) */
    int16_t  bat_current_ma;    /* Battery average current (mA; negative=discharging) */
    bool     battery_present;   /* BAT_DET flag */
    bool     fully_charged;     /* FC flag */
    bool     critical_low;      /* SOCF flag */

    /* From BQ25892 (charger) */
    uint8_t  chrg_status;       /* 0=off,1=pre,2=fast,3=done (bq25892_chrg_status_t) */
    bool     power_good;        /* Valid VBUS present */
    bool     charge_enabled;    /* Current CHG_CONFIG state */

    /* From INA230 (VSYS monitor) */
    uint16_t vsys_mv;           /* VSYS rail voltage (mV) */
    int16_t  isys_ma;           /* VSYS current consumption (mA) */
} pwr_source_status_t;

/**
 * @brief Initialize all power management ICs and GPIO.
 *        Requires i2c_dev_support_init() to be called first.
 * @return ESP_OK on success
 */
esp_err_t pwr_source_init(void);

/**
 * @brief Enable or disable battery charging (drives BC_CE# GPIO + BQ25892 CHG_CONFIG).
 * @param enable true to start charging, false to stop
 * @return ESP_OK on success
 */
esp_err_t pwr_source_set_charge_enable(bool enable);

/**
 * @brief Enable or disable OTG boost (battery → VBUS).
 * @param enable true to enable OTG
 * @return ESP_OK on success
 */
esp_err_t pwr_source_set_otg(bool enable);

/**
 * @brief Read a combined snapshot from all 3 ICs.
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t pwr_source_get_status(pwr_source_status_t *status);

/**
 * @brief Threshold-based charge control — call periodically (e.g. every 30 s).
 *        Stops charging if vbat > UPPER_THRESHOLD, enables charging if vbat < LOWER_THRESHOLD.
 * @return ESP_OK on success
 */
esp_err_t pwr_source_charge_monitor(void);

/**
 * @brief Read battery SoC directly (convenience wrapper over bq27441_read_soc_pct).
 * @param soc_pct Output SoC 0–100
 * @return ESP_OK on success
 */
esp_err_t pwr_source_get_soc(uint8_t *soc_pct);

/**
 * @brief GPIO ISR handler for BC_INT (charger fault/event interrupt).
 *        Register this as a rising-edge ISR on BC_INT_GPIO_NUM.
 */
void pwr_source_int_handler(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* PWR_SOURCE_HANDLER_H */

/* Legacy note: old pwr_source_set_1v8/3v3/5v0 are removed.
 * The new board no longer requires TCA-based power rail switching.
 * All system rails (1.8V, 3.3V, 5V) are always-on once VSYS is active.
 */
