/**
 * @file pwr_source_handler.c
 * @brief Power Management Coordinator — BQ25892 + INA230 + BQ27441
 *
 * Replaces the old TCA6424A rail-switching logic.
 * System rails (1.8V / 3.3V / 5V) are always-on once VSYS is active.
 */

#include "pwr_source_handler.h"
#include "bq25892_handler.h"
#include "ina230_handler.h"
#include "bq27441_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "PWR_SOURCE";

/* Semaphore posted from BC_INT ISR to the caller task */
static SemaphoreHandle_t s_int_sem = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static void iox_output_init(stack_gpio_pin_num_t pin, bool default_level)
{
    if (pin == STACK_GPIO_PIN_NONE) return;
    stack_handler_gpio_set_direction(0, pin, true);
    stack_handler_gpio_write(0, pin, default_level);
}

static void iox_input_init(stack_gpio_pin_num_t pin)
{
    if (pin == STACK_GPIO_PIN_NONE) return;
    stack_handler_gpio_set_direction(0, pin, false);
}

/* Global state for charging hysteresis (prevents on/off oscillation) */
static bool s_charge_enabled_state = true;
static bool s_battery_enabled_state = true;  /* true = battery FET connected (default) */

/* Debounce: require N consecutive readings before toggling charge state */
#define CHARGE_DEBOUNCE_COUNT       3    /* 3 consecutive reads (= 15s at 5s interval) */
#define CHARGE_MIN_TOGGLE_INTERVAL_MS 15000  /* minimum 15s between charge state changes */
static int s_charge_stop_counter  = 0;   /* counts consecutive "should stop" readings */
static int s_charge_start_counter = 0;   /* counts consecutive "should start" readings */
static TickType_t s_last_charge_toggle_tick = 0;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t pwr_source_init(void)
{
    ESP_LOGI(TAG, "Initializing power management ICs");

    /* BC IOX — output pins */
    iox_output_init(BC_CE_IOX_PIN,   false);   /* BC_CE# LOW = charging enabled */
    s_charge_enabled_state = true;             /* Initialize charging state for hysteresis */
    
    iox_output_init(BC_OTG_IOX_PIN,  false);   /* OTG off */
    iox_output_init(BC_PSEL_IOX_PIN, false);   /* PSEL default */

    /* BC IOX — input pins */
    iox_input_init(BC_INT_IOX_PIN);
    iox_input_init(BC_STAT_IOX_PIN);
    iox_input_init(BC_PG_IOX_PIN);

    /* TCA6416A IO expander does not directly support attach interrupt
       without tca_handler implementation. Charger status is polled instead. */
    s_int_sem = NULL;

    /* Initialize battery charger */
    esp_err_t ret = bq25892_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BQ25892 init failed: %s (continuing)", esp_err_to_name(ret));
    }

    /* Initialize system power monitor */
    ret = ina230_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA230 init failed: %s (continuing)", esp_err_to_name(ret));
    }

    /* Initialize fuel gauge */
    ret = bq27441_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BQ27441 init failed: %s (continuing)", esp_err_to_name(ret));
    } else {
        /* Reprogram capacity if needed.
         * bq27441_reprogram_capacity() internally checks if capacity already matches
         * the target (3000 mAh) and skips the write if so. We call it unconditionally
         * to avoid the read_design_capacity() sequence which starts IT and blocks CFGUPDATE.
         */
        esp_err_t cap_ret = bq27441_reprogram_capacity(3000);
        if (cap_ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ BQ27441 capacity check/reprogram complete");
        } else if (cap_ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "⚠ BQ27441 capacity reprogram timeout (IT interference?) — capacity unknown");
        } else {
            ESP_LOGW(TAG, "⚠ BQ27441 capacity reprogram: %s", esp_err_to_name(cap_ret));
        }
    }

    /* Log initial battery status */
    uint8_t soc = 0;
    uint16_t vbat = 0;
    bq27441_read_soc_pct(&soc);
    bq27441_read_voltage_mv(&vbat);
    ESP_LOGI(TAG, "Battery: %u mV, SoC: %u%%", vbat, soc);

    ESP_LOGI(TAG, "Power management initialized");
    return ESP_OK;
}

esp_err_t pwr_source_set_battery_enable(bool enable)
{
    s_battery_enabled_state = enable;
    /* BATFET_DIS=1 disconnects, so pass !enable */
    esp_err_t ret = bq25892_set_batfet_disable(!enable);
    ESP_LOGI(TAG, "Battery source %s", enable ? "ENABLED" : "DISABLED");
    return ret;
}

esp_err_t pwr_source_set_charge_enable(bool enable)
{
    /* Update charging state for hysteresis logic */
    s_charge_enabled_state = enable;

    /* Drive BC_CE# IOX: active-low, so LOW=enable, HIGH=disable */
    if (BC_CE_IOX_PIN != STACK_GPIO_PIN_NONE) {
        stack_handler_gpio_write(0, BC_CE_IOX_PIN, enable ? false : true);
    }
    /* Also write BQ25892 CHG_CONFIG register */
    esp_err_t ret = bq25892_set_charge_enable(enable);
    ESP_LOGI(TAG, "Charging %s", enable ? "ENABLED" : "DISABLED");
    return ret;
}

esp_err_t pwr_source_set_otg(bool enable)
{
    if (BC_OTG_IOX_PIN != STACK_GPIO_PIN_NONE) {
        stack_handler_gpio_write(0, BC_OTG_IOX_PIN, enable ? true : false);
    }
    return bq25892_set_otg(enable);
}

esp_err_t pwr_source_get_status(pwr_source_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    /* BQ27441 — fuel gauge */
    bq27441_status_t fg = {0};
    esp_err_t ret = bq27441_read_status(&fg);
    if (ret == ESP_OK) {
        status->vbat_mv         = fg.voltage_mv;
        status->soc_pct         = fg.soc_pct;
        status->bat_current_ma  = fg.avg_current_ma;
        status->battery_present = fg.battery_present;
        status->fully_charged   = fg.fully_charged;
        status->critical_low    = fg.critical_low;

        /* Fallback: Use OCV-based SoC estimate if BQ27441's IT algorithm failed (SoC=0%).
           This happens when INITCOMP is not set (IT not initialized on this chip). */
        if (status->soc_pct == 0 && fg.voltage_mv > 3000) {
            status->soc_pct = bq27441_estimate_soc_from_ocv(fg.voltage_mv);
            if (status->soc_pct > 0) {
                ESP_LOGD(TAG, "Using OCV-based SoC: %u%% (Vbat=%u mV, IT_ALGO failed)",
                        status->soc_pct, fg.voltage_mv);
            }
        }
    }

    /* BQ25892 — charger */
    bq25892_status_t chg = {0};
    ret = bq25892_read_status(&chg);
    if (ret == ESP_OK) {
        status->chrg_status    = (uint8_t)chg.chrg_status;
        status->power_good     = chg.power_good;
        status->charge_enabled = chg.charge_enabled;
    }

    /* INA230 — VSYS monitor */
    ina230_status_t sys = {0};
    ret = ina230_read_status(&sys);
    if (ret == ESP_OK) {
        status->vsys_mv  = sys.bus_voltage_mv;
        status->isys_ma  = sys.current_ma;
    }

    return ESP_OK;
}

esp_err_t pwr_source_charge_monitor_with_status(const pwr_source_status_t *status)
{
    uint8_t soc_pct;
    uint16_t vbat_mv;

    if (status) {
        /* Use pre-read status (avoids double I2C read in same cycle) */
        soc_pct = status->soc_pct;
        vbat_mv = status->vbat_mv;
    } else {
        /* Fallback: read directly */
        esp_err_t ret = bq27441_read_soc_pct(&soc_pct);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Cannot read SoC");
            return ret;
        }
        bq27441_read_voltage_mv(&vbat_mv);

        /* Apply OCV fallback (same logic as pwr_source_get_status) */
        if (soc_pct == 0 && vbat_mv > 3000) {
            soc_pct = bq27441_estimate_soc_from_ocv(vbat_mv);
        }
    }

    /* Sanity check: if SoC reads 100% but voltage is below 3.9V, IT is lying.
     * If SoC reads 0% but voltage is above 3.5V, IT is lying.
     * In both cases, use OCV estimate instead. */
    if (soc_pct >= 100 && vbat_mv < 3900) {
        uint8_t ocv_soc = bq27441_estimate_soc_from_ocv(vbat_mv);
        ESP_LOGW(TAG, "CHARGE_CTRL: SoC=%u%% but Vbat=%u mV too low — using OCV estimate %u%%",
                 soc_pct, vbat_mv, ocv_soc);
        soc_pct = ocv_soc;
    } else if (soc_pct == 0 && vbat_mv > 3500) {
        uint8_t ocv_soc = bq27441_estimate_soc_from_ocv(vbat_mv);
        ESP_LOGW(TAG, "CHARGE_CTRL: SoC=0%% but Vbat=%u mV — using OCV estimate %u%%",
                 vbat_mv, ocv_soc);
        soc_pct = ocv_soc;
    }

    /* Minimum toggle interval: prevent rapid on/off cycling */
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed_since_toggle = now - s_last_charge_toggle_tick;
    bool toggle_allowed = (elapsed_since_toggle >= pdMS_TO_TICKS(CHARGE_MIN_TOGGLE_INTERVAL_MS))
                          || (s_last_charge_toggle_tick == 0);  /* allow first toggle */

    /* Charge termination strategy:
     *   PRIMARY:   BQ25892 hardware DONE (chrg_status==3) — charger IC is authoritative
     *   SECONDARY: SoC >= 100% AND Vbat >= PWR_BATT_CHARGE_MIN_STOP_MV
     *   NEVER stop if Vbat < PWR_BATT_CHARGE_MIN_STOP_MV (BQ27441 SoC is unreliable
     *   until a full charge-discharge calibration cycle has completed).
     *
     * Resume strategy:
     *   SoC drops below PWR_BATT_CHARGE_RESUME_SOC_PCT  OR
     *   Vbat drops below PWR_BATT_CHARGE_RESUME_MV
     *
     * Both directions require CHARGE_DEBOUNCE_COUNT consecutive readings.
     */
    uint8_t chrg_status = (status) ? status->chrg_status : 0;

    if (s_charge_enabled_state) {
        /* === Charging is ON — decide if we should STOP === */
        bool should_stop = false;
        const char *stop_reason = "";

        if (chrg_status == 3) {
            /* BQ25892 says charge DONE — hardware termination (most reliable) */
            should_stop = true;
            stop_reason = "BQ25892 DONE";
        } else if (soc_pct >= PWR_BATT_CHARGE_FULL_SOC_PCT &&
                   vbat_mv >= PWR_BATT_CHARGE_MIN_STOP_MV) {
            /* SoC says full AND voltage confirms it */
            should_stop = true;
            stop_reason = "SoC+Voltage";
        }

        /* Safety: NEVER stop charging if voltage is below minimum */
        if (should_stop && vbat_mv < PWR_BATT_CHARGE_MIN_STOP_MV) {
            ESP_LOGW(TAG, "CHARGE_CTRL: Would stop (%s) but Vbat=%u mV < %u mV — keep charging",
                     stop_reason, vbat_mv, PWR_BATT_CHARGE_MIN_STOP_MV);
            should_stop = false;
        }

        if (should_stop) {
            s_charge_stop_counter++;
            s_charge_start_counter = 0;
            if (s_charge_stop_counter >= CHARGE_DEBOUNCE_COUNT && toggle_allowed) {
                ESP_LOGI(TAG, "CHARGE_CTRL: Battery full (%s, SoC=%u%%, Vbat=%u mV, %d consecutive) — stopping",
                         stop_reason, soc_pct, vbat_mv, s_charge_stop_counter);
                s_charge_enabled_state = false;
                pwr_source_set_charge_enable(false);
                s_charge_stop_counter = 0;
                s_last_charge_toggle_tick = now;
            } else {
                ESP_LOGD(TAG, "CHARGE_CTRL: should_stop (%s) — debounce %d/%d%s",
                        stop_reason, s_charge_stop_counter, CHARGE_DEBOUNCE_COUNT,
                        toggle_allowed ? "" : " (cooldown)");
            }
        } else {
            s_charge_stop_counter = 0;
            ESP_LOGD(TAG, "CHARGE_CTRL: ON, SoC=%u%% Vbat=%u mV chrg_st=%u",
                    soc_pct, vbat_mv, chrg_status);
        }
    } else {
        /* === Charging is OFF — decide if we should RESUME === */
        bool should_resume = false;
        const char *resume_reason = "";

        /* Use VOLTAGE as the sole resume criterion.
         *
         * Why NOT SoC: After a full charge cycle (e.g. stopped at Vbat=4136 mV),
         * the battery's open-circuit voltage settles to ~4075 mV once current drops
         * to 0. The BQ27441 computes OCV-based SoC from this lower resting voltage
         * and reports ~87%. Using SoC <= 95% as a resume trigger would immediately
         * restart charging, creating an oscillation loop:
         *   stop (4136 mV, SoC=100%) → rest (4075 mV, SoC=87%) → resume → repeat
         *
         * Voltage is the correct physical criterion: resume charging only when the
         * battery has genuinely discharged below PWR_BATT_CHARGE_RESUME_MV.
         * At Vbat=4075 mV OCV, the battery is still ~87-90% full — no need to charge.
         */
        if (vbat_mv > 0 && vbat_mv < PWR_BATT_CHARGE_RESUME_MV) {
            should_resume = true;
            resume_reason = "Voltage low";
        }

        if (should_resume) {
            s_charge_start_counter++;
            s_charge_stop_counter = 0;
            if (s_charge_start_counter >= CHARGE_DEBOUNCE_COUNT && toggle_allowed) {
                ESP_LOGI(TAG, "CHARGE_CTRL: %s (SoC=%u%%, Vbat=%u mV < %u mV, %d consecutive) — resuming",
                         resume_reason, soc_pct, vbat_mv, PWR_BATT_CHARGE_RESUME_MV,
                         s_charge_start_counter);
                s_charge_enabled_state = true;
                pwr_source_set_charge_enable(true);
                s_charge_start_counter = 0;
                s_last_charge_toggle_tick = now;
            } else {
                ESP_LOGD(TAG, "CHARGE_CTRL: should_resume (%s) — debounce %d/%d%s",
                        resume_reason, s_charge_start_counter, CHARGE_DEBOUNCE_COUNT,
                        toggle_allowed ? "" : " (cooldown)");
            }
        } else {
            s_charge_start_counter = 0;
            ESP_LOGD(TAG, "CHARGE_CTRL: OFF, SoC=%u%% Vbat=%u mV (resume when Vbat < %u mV)",
                    soc_pct, vbat_mv, PWR_BATT_CHARGE_RESUME_MV);
        }
    }

    /* Also handle low battery critical alert (independent of charging state) */
    if (vbat_mv > 0 && vbat_mv <= PWR_BATT_LOWER_THRESHOLD_MV) {
        ESP_LOGW(TAG, "Critical low battery: %u mV <= %u mV",
                 vbat_mv, PWR_BATT_LOWER_THRESHOLD_MV);
    }

    return ESP_OK;
}

esp_err_t pwr_source_charge_monitor(void)
{
    /* Legacy API: read status and delegate */
    pwr_source_status_t status = {0};
    esp_err_t ret = pwr_source_get_status(&status);
    if (ret != ESP_OK) return ret;
    return pwr_source_charge_monitor_with_status(&status);
}

esp_err_t pwr_source_get_soc(uint8_t *soc_pct)
{
    if (!soc_pct) return ESP_ERR_INVALID_ARG;
    return bq27441_read_soc_pct(soc_pct);
}

void IRAM_ATTR pwr_source_int_handler(void *arg)
{
    /* Post from ISR; a monitor task should take the semaphore and read REG09 */
    if (s_int_sem) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_int_sem, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}
