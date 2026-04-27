/**
 * @file pwr_monitor_task.c
 * @brief Power Monitor Task — Battery Status Monitor & HMI Display Update
 *
 * Periodically monitors battery status and updates HMI display.
 * Implements threshold-based charge control logic.
 */

#include "pwr_monitor_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hmi_task.h"
#include "pwr_source_handler.h"
#include <string.h>

static const char *TAG = "PWR_MON";

/* ================================================================== */
/*  Static State                                                       */
/* ================================================================== */

/* Global published status */
pwr_monitor_status_t g_pwr_monitor_status = {0};
SemaphoreHandle_t g_pwr_monitor_mutex = NULL;

static TaskHandle_t s_monitor_task = NULL;
static volatile bool s_monitor_running = false;

/* Power-good change callback */
static void (*s_power_good_cb)(bool) = NULL;
static bool s_last_power_good = true; /* optimistic: assume VBUS at boot */

/* ================================================================== */
/*  Power-Good Callback Registration                                  */
/* ================================================================== */

void pwr_monitor_register_power_good_cb(void (*cb)(bool power_good)) {
  s_power_good_cb = cb;
  /* Sync the last-known state so the first real reading produces a
   * transition event if VBUS is already absent at registration time. */
  s_last_power_good = true; /* will be corrected on the next task cycle */
}

/* ================================================================== */
/*  Internal Helpers                                                   */
/* ================================================================== */

/**
 * @brief Format battery SoC as color category for HMI.
 *        - >= 50%: Green (healthy)
 *        - >= 20%: Yellow (warning)
 *        - >= 10%: Orange (critical)
 *        - <  10%: Red (emergency)
 */
static const char *get_soc_color_name(uint8_t soc_pct) __attribute__((unused));
static const char *get_soc_color_name(uint8_t soc_pct) {
  if (soc_pct >= 50)
    return "GREEN";
  if (soc_pct >= 20)
    return "YELLOW";
  if (soc_pct >= 10)
    return "ORANGE";
  return "RED";
}

/**
 * @brief Log battery status (debug output).
 */
static void log_battery_status(const pwr_monitor_status_t *s) {
  ESP_LOGI(TAG,
           "Battery: SoC=%u%% Vbat=%u mV I=%d mA | "
           "VSYS=%u mV Isys=%d mA | Chrg=%s",
           s->bat_soc_pct, s->bat_voltage_mv, s->bat_current_ma,
           s->vsys_voltage_mv, s->isys_current_ma,
           s->bat_is_charging ? "ON" : "OFF");
}

/**
 * @brief Update HMI display with current battery status.
 *        Called from the monitor task to refresh home page stats.
 */
static void update_hmi_battery_status(const pwr_monitor_status_t *s) {
  if (!hmi_is_active()) {
    return; /* HMI not in active mode, skip */
  }

  /* Build HMI status structure for display update */
  hmi_status_t hmi_status = {0};

  /* Battery section */
  hmi_status.bat_soc = s->bat_soc_pct;
  hmi_status.bat_is_charging = s->bat_is_charging;
  hmi_status.bat_voltage_mv = s->bat_voltage_mv;

  /* WiFi and LTE sections are not updated by power monitor.
     HMI handler maintains these from other sources (wifi_connect, lte_connect).
     For now, copy from cached state if available. */

  /* Call HMI refresher — will update battery %, charging indicator, etc. */
  hmi_refresh_status(&hmi_status);
}

/**
 * @brief Read all power source status into monitor structure.
 */
static esp_err_t read_power_status(pwr_monitor_status_t *status) {
  if (!status)
    return ESP_ERR_INVALID_ARG;

  /* Read unified power status from handler */
  pwr_source_status_t pwr_status = {0};
  esp_err_t ret = pwr_source_get_status(&pwr_status);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "pwr_source_get_status failed: %s", esp_err_to_name(ret));
    return ret;
  }

  /* Map to monitor status */
  status->bat_soc_pct = pwr_status.soc_pct;
  status->bat_voltage_mv = pwr_status.vbat_mv;
  status->bat_current_ma = pwr_status.bat_current_ma;
  status->bat_present = pwr_status.battery_present;
  status->bat_fully_charged = pwr_status.fully_charged;
  status->bat_critical_low = pwr_status.critical_low;
  status->chrg_status = pwr_status.chrg_status;
  status->power_good = pwr_status.power_good;
  status->vsys_voltage_mv = pwr_status.vsys_mv;
  status->isys_current_ma = pwr_status.isys_ma;

  /* Derive charging flag from charger status */
  status->bat_is_charging =
      (pwr_status.chrg_status == 2 || pwr_status.chrg_status == 1);

  status->timestamp_ms = xTaskGetTickCount();

  return ESP_OK;
}

/* ================================================================== */
/*  Monitor Task                                                       */
/* ================================================================== */

static void power_monitor_task(void *arg) {
  ESP_LOGI(TAG, "Power monitor task started");
  s_monitor_running = true;

  const TickType_t update_interval =
      pdMS_TO_TICKS(PWR_MONITOR_UPDATE_INTERVAL_MS);

  while (s_monitor_running) {
    TickType_t cycle_start = xTaskGetTickCount();

    /* 1. Read power status from all ICs */
    pwr_monitor_status_t new_status = {0};
    esp_err_t ret = read_power_status(&new_status);

    if (ret == ESP_OK) {
      /* 2. Call charge monitor with already-read status (avoids double I2C
       * read). Build a pwr_source_status_t from the monitor status to pass
       * through. */
      {
        pwr_source_status_t src_status = {
            .vbat_mv = new_status.bat_voltage_mv,
            .soc_pct = new_status.bat_soc_pct,
            .bat_current_ma = new_status.bat_current_ma,
            .battery_present = new_status.bat_present,
            .fully_charged = new_status.bat_fully_charged,
            .critical_low = new_status.bat_critical_low,
            .chrg_status = new_status.chrg_status,
            .power_good = new_status.power_good,
        };
        pwr_source_charge_monitor_with_status(&src_status);
      }

      /* 3. Update global status under mutex */
      if (g_pwr_monitor_mutex) {
        if (xSemaphoreTake(g_pwr_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          memcpy(&g_pwr_monitor_status, &new_status, sizeof(new_status));
          xSemaphoreGive(g_pwr_monitor_mutex);
        }
      }

      /* 4. Log battery status */
      if (is_power_sampling)
        log_battery_status(&new_status);

      /* 5. Notify on VBUS power-good state change */
      if (new_status.power_good != s_last_power_good) {
        s_last_power_good = new_status.power_good;
        if (s_power_good_cb) {
          s_power_good_cb(new_status.power_good);
        }
      }

      /* 6. Update HMI display if active */
      update_hmi_battery_status(&new_status);
    }

    /* 7. Sleep until next update (compensate for time spent reading) */
    TickType_t elapsed = xTaskGetTickCount() - cycle_start;
    if (elapsed < update_interval) {
      vTaskDelay(update_interval - elapsed);
    }
  }

  s_monitor_running = false;
  ESP_LOGI(TAG, "Power monitor task stopped");
  vTaskDelete(NULL);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t pwr_monitor_task_start(void) {
  if (s_monitor_task != NULL) {
    ESP_LOGW(TAG, "Monitor task already running");
    return ESP_OK;
  }

  /* Create mutex if not already done */
  if (g_pwr_monitor_mutex == NULL) {
    g_pwr_monitor_mutex = xSemaphoreCreateMutex();
    if (g_pwr_monitor_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  /* Spawn monitor task */
  s_monitor_running = true;
  BaseType_t ret = xTaskCreate(power_monitor_task, "pwr_monitor",
                               PWR_MONITOR_TASK_STACK_SIZE, NULL,
                               PWR_MONITOR_TASK_PRIORITY, &s_monitor_task);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create monitor task");
    s_monitor_running = false;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Power monitor task started");
  return ESP_OK;
}

void pwr_monitor_task_stop(void) {
  if (s_monitor_task == NULL) {
    ESP_LOGW(TAG, "Monitor task not running");
    return;
  }

  s_monitor_running = false;

  /* Wait for task to exit (max 1 second) */
  for (int i = 0; i < 20 && s_monitor_running; i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  s_monitor_task = NULL;
  ESP_LOGI(TAG, "Power monitor task stopped");
}

bool pwr_monitor_is_running(void) { return s_monitor_running; }

esp_err_t pwr_monitor_get_status(pwr_monitor_status_t *status) {
  if (!status)
    return ESP_ERR_INVALID_ARG;

  if (g_pwr_monitor_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_pwr_monitor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(status, &g_pwr_monitor_status, sizeof(*status));
  xSemaphoreGive(g_pwr_monitor_mutex);

  return ESP_OK;
}

esp_err_t pwr_monitor_update_now(void) {
  pwr_monitor_status_t new_status = {0};
  esp_err_t ret = read_power_status(&new_status);

  if (ret == ESP_OK) {
    /* Use pre-read status for charge monitor */
    pwr_source_status_t src_status = {
        .vbat_mv = new_status.bat_voltage_mv,
        .soc_pct = new_status.bat_soc_pct,
        .bat_current_ma = new_status.bat_current_ma,
        .battery_present = new_status.bat_present,
        .fully_charged = new_status.bat_fully_charged,
        .critical_low = new_status.bat_critical_low,
        .chrg_status = new_status.chrg_status,
        .power_good = new_status.power_good,
    };
    pwr_source_charge_monitor_with_status(&src_status);

    if (g_pwr_monitor_mutex) {
      if (xSemaphoreTake(g_pwr_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&g_pwr_monitor_status, &new_status, sizeof(new_status));
        xSemaphoreGive(g_pwr_monitor_mutex);
      }
    }

    update_hmi_battery_status(&new_status);
  }

  return ret;
}
