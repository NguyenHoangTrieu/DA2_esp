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

esp_err_t pwr_source_charge_monitor(void)
{
    uint8_t soc_pct = 0;
    esp_err_t ret = bq27441_read_soc_pct(&soc_pct);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read SoC");
        return ret;
    }

    /* Hysteresis logic (SoC-based): prevent on/off oscillation around threshold
       - If charging is enabled: stop when reaching FULL_SOC_PCT
       - If charging is disabled: resume only when dropping below RESUME_SOC_PCT (hysteresis)
     */
    if (s_charge_enabled_state) {
        /* Charging is currently ON — stop when battery is full (100% SoC) */
        if (soc_pct >= PWR_BATT_CHARGE_FULL_SOC_PCT) {
            ESP_LOGI(TAG, "CHARGE_CTRL: Battery full (SoC=%u%% >= %u%%) — stopping charge",
                     soc_pct, PWR_BATT_CHARGE_FULL_SOC_PCT);
            s_charge_enabled_state = false;
            pwr_source_set_charge_enable(false);
        } else {
            ESP_LOGD(TAG, "CHARGE_CTRL: State=ON, SoC=%u%% (full_thres=%u%%, resume_hyst=%u%%)",
                    soc_pct, PWR_BATT_CHARGE_FULL_SOC_PCT, PWR_BATT_CHARGE_RESUME_SOC_PCT);
        }
    } else {
        /* Charging is currently OFF — resume only if SoC drops enough (hysteresis) */
        if (soc_pct <= PWR_BATT_CHARGE_RESUME_SOC_PCT) {
            ESP_LOGI(TAG, "CHARGE_CTRL: SoC dropped to %u%% (below %u%% hyst) — resuming charge",
                     soc_pct, PWR_BATT_CHARGE_RESUME_SOC_PCT);
            s_charge_enabled_state = true;
            pwr_source_set_charge_enable(true);
        } else {
            ESP_LOGD(TAG, "CHARGE_CTRL: State=OFF, SoC=%u%% (waiting for hyst=%u%%)",
                    soc_pct, PWR_BATT_CHARGE_RESUME_SOC_PCT);
        }
    }

    /* Also handle low battery critical alert (independent of charging state) */
    uint16_t vbat_mv = 0;
    esp_err_t voltage_ret = bq27441_read_voltage_mv(&vbat_mv);
    if (voltage_ret == ESP_OK && vbat_mv <= PWR_BATT_LOWER_THRESHOLD_MV) {
        ESP_LOGW(TAG, "Critical low battery: %u mV <= %u mV",
                 vbat_mv, PWR_BATT_LOWER_THRESHOLD_MV);
    }

    return ESP_OK;
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
