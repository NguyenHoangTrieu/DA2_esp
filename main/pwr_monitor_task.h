/**
 * @file pwr_monitor_task.h
 * @brief Power Monitor Task — Battery Status Monitor & HMI Display Update
 *
 * Periodically monitors battery status (voltage, SoC, charge) and updates HMI display.
 * Implements threshold-based charge control via pwr_source_handler.
 *
 * ## Task Lifecycle
 *
 * ```
 * pwr_monitor_task_start()
 *   ↓
 * [Monitor Task Loop — running at priority 4, interval 500 ms]
 *   ├─ Read BQ27441 (SoC%, voltage, current, flags)
 *   ├─ Read BQ25892 (charge status, power good)
 *   ├─ Read INA230 (VSYS voltage, system current)
 *   ├─ Call pwr_source_charge_monitor() for threshold check (4.1V / 3.5V)
 *   ├─ Publish status to g_pwr_monitor_status (HMI reads this)
 *   ├─ If HMI active: call hmi_refresh_status()
 *   └─ Sleep 500 ms, repeat
 *
 * pwr_monitor_task_stop()
 *   ↓
 * [Task deleted]
 * ```
 */

#ifndef PWR_MONITOR_TASK_H
#define PWR_MONITOR_TASK_H

#include "esp_err.h"
#include "pwr_source_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "DA2_esp.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Configuration                                                      */
/* ================================================================== */

#define PWR_MONITOR_TASK_STACK_SIZE 4096
#define PWR_MONITOR_TASK_PRIORITY   4       /* Medium priority */
#define PWR_MONITOR_UPDATE_INTERVAL_MS 1000  /* 1000 ms update */

/* ================================================================== */
/*  Public Data Structures                                             */
/* ================================================================== */

/**
 * @brief Published battery status for HMI display.
 *        This structure is updated by the power monitor task and read by HMI.
 */
typedef struct {
    /* Battery */
    uint8_t  bat_soc_pct;           /* State of Charge: 0–100 % */
    uint16_t bat_voltage_mv;        /* Battery voltage in mV */
    int16_t  bat_current_ma;        /* Battery current in mA (negative = discharging) */
    bool     bat_present;           /* Battery inserted and detected */
    bool     bat_is_charging;       /* High = charger actively charging */
    bool     bat_fully_charged;     /* High = charge complete (4.1V reached) */
    bool     bat_critical_low;      /* High = battery SoC critical (<5%?) */

    /* Charger */
    uint8_t  chrg_status;           /* BQ25892 CHRG_STAT: 0=notchg, 1=prechg, 2=fastchg, 3=done */
    bool     power_good;            /* VBUS present and valid */

    /* System Rail */
    uint16_t vsys_voltage_mv;       /* +4V2_VSYS rail voltage (INA230) */
    int16_t  isys_current_ma;       /* System current draw (positive = load, negative = charging) */

    /* Timestamp */
    uint32_t timestamp_ms;          /* milliseconds since boot when status was captured */
} pwr_monitor_status_t;

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

/**
 * @brief Global battery status (published by monitor task, read by HMI).
 *        Thread-safe: protected by mutex g_pwr_monitor_mutex.
 */
extern pwr_monitor_status_t g_pwr_monitor_status;

/**
 * @brief Mutex protecting g_pwr_monitor_status during read/write.
 */
extern SemaphoreHandle_t g_pwr_monitor_mutex;

/**
 * @brief Start the power monitor task.
 *        Spawns a FreeRTOS task that periodically reads battery ICs and updates HMI.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t pwr_monitor_task_start(void);

/**
 * @brief Stop the power monitor task.
 *        Gracefully stops the monitor task and releases resources.
 */
void pwr_monitor_task_stop(void);

/**
 * @brief Check if power monitor task is running.
 *
 * @return true if running, false otherwise
 */
bool pwr_monitor_is_running(void);

/**
 * @brief Read the latest battery status snapshot (thread-safe).
 *        Acquires mutex to copy current status.
 *
 * @param[out] status Destination buffer (must not be NULL)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL
 */
esp_err_t pwr_monitor_get_status(pwr_monitor_status_t *status);

/**
 * @brief Force an immediate status update and HMI refresh.
 *        Normally updates occur on the monitor task timer.
 *        Use this for immediate feedback after user action (e.g., charging started).
 *
 * @return ESP_OK on success
 */
esp_err_t pwr_monitor_update_now(void);

/**
 * @brief Register a callback invoked whenever the VBUS power-good state changes.
 *        Called from the monitor task context; must be ISR-safe (no long blocking).
 *        Pass NULL to deregister.
 * @param cb  void(*)(bool power_good) — true = VBUS present, false = battery only
 */
void pwr_monitor_register_power_good_cb(void (*cb)(bool power_good));

#ifdef __cplusplus
}
#endif

#endif /* PWR_MONITOR_TASK_H */
