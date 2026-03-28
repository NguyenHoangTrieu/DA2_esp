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

static void gpio_output_init(gpio_num_t pin, int default_level)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, default_level);
}

static void gpio_input_init(gpio_num_t pin, gpio_int_type_t intr)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = intr,
    };
    gpio_config(&cfg);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t pwr_source_init(void)
{
    ESP_LOGI(TAG, "Initializing power management ICs");

    /* BC discrete GPIO — output pins */
    gpio_output_init(BC_CE_GPIO_NUM,   0);   /* BC_CE# LOW = charging enabled */
    gpio_output_init(BC_OTG_GPIO_NUM,  0);   /* OTG off */
    gpio_output_init(BC_PSEL_GPIO_NUM, 0);   /* PSEL default */

    /* BC discrete GPIO — input pins */
    gpio_input_init(BC_INT_GPIO_NUM,  GPIO_INTR_NEGEDGE);
    gpio_input_init(BC_STAT_GPIO_NUM, GPIO_INTR_DISABLE);
    gpio_input_init(BC_PG_GPIO_NUM,   GPIO_INTR_DISABLE);

    /* Register BC_INT ISR if pin is configured */
    if (BC_INT_GPIO_NUM != GPIO_NUM_NC) {
        s_int_sem = xSemaphoreCreateBinary();
        gpio_isr_handler_add(BC_INT_GPIO_NUM, pwr_source_int_handler, NULL);
    }

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

esp_err_t pwr_source_set_charge_enable(bool enable)
{
    /* Drive BC_CE# GPIO: active-low, so LOW=enable, HIGH=disable */
    if (BC_CE_GPIO_NUM != GPIO_NUM_NC) {
        gpio_set_level(BC_CE_GPIO_NUM, enable ? 0 : 1);
    }
    /* Also write BQ25892 CHG_CONFIG register */
    esp_err_t ret = bq25892_set_charge_enable(enable);
    ESP_LOGI(TAG, "Charging %s", enable ? "ENABLED" : "DISABLED");
    return ret;
}

esp_err_t pwr_source_set_otg(bool enable)
{
    if (BC_OTG_GPIO_NUM != GPIO_NUM_NC) {
        gpio_set_level(BC_OTG_GPIO_NUM, enable ? 1 : 0);
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
    uint16_t vbat_mv = 0;
    esp_err_t ret = bq27441_read_voltage_mv(&vbat_mv);
    if (ret != ESP_OK) {
        /* Fall back to BQ25892 ADC if fuel gauge unavailable */
        ret = bq25892_read_batv_mv(&vbat_mv);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Cannot read battery voltage");
            return ret;
        }
    }

    if (vbat_mv >= PWR_BATT_UPPER_THRESHOLD_MV) {
        ESP_LOGI(TAG, "Battery full (%u mV >= %u mV) — stopping charge",
                 vbat_mv, PWR_BATT_UPPER_THRESHOLD_MV);
        pwr_source_set_charge_enable(false);
    } else if (vbat_mv <= PWR_BATT_LOWER_THRESHOLD_MV) {
        ESP_LOGW(TAG, "Battery low (%u mV <= %u mV) — enabling charge",
                 vbat_mv, PWR_BATT_LOWER_THRESHOLD_MV);
        pwr_source_set_charge_enable(true);
    }

    /* BQ25892 hardware VREG at 4.096V also limits charge voltage autonomously */
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
