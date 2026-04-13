/**
 * @file hmi_task.h
 * @brief HMI Application Task — FreeRTOS wrapper for HMI mode management
 *
 * This is the Application layer of the three-tier HMI architecture:
 *
 *   ┌──────────────────────────────────────────┐
 *   │  Application  │  hmi_task.h / hmi_task.c │  FreeRTOS task, mode FSM
 *   ├──────────────────────────────────────────┤
 *   │  Middleware   │  hmi_display.h / .c      │  TJC protocol, page logic
 *   ├──────────────────────────────────────────┤
 *   │  BSP          │  hmi_handler.h / .c      │  Raw UART2 driver
 *   └──────────────────────────────────────────┘
 *
 * Responsibilities of this layer:
 *   • Manage HMI active/inactive state
 *   • Control UART switch (GPIO46) and BSP init/deinit
 *   • Run an RX FreeRTOS task that feeds frames to the Middleware
 *   • Provide a thread-safe status-update interface for pwr_monitor_task
 *
 * Usage:
 *   1. hmi_task_init()          — call once at boot (before scheduler starts)
 *   2. hmi_task_enter_mode()    — switch GPIO46, install UART, start RX task,
 *                                 navigate display to home page
 *   3. hmi_task_update_status() — called by pwr_monitor_task every 5 s
 *   4. hmi_task_exit_mode()     — stop RX task, deinit UART, release GPIO46
 */

#ifndef HMI_TASK_H
#define HMI_TASK_H

#include "hmi_display.h"   /* hmi_status_t, page IDs, colour constants */
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief One-time initialisation (resets state, creates the status mutex).
 *        Call before the FreeRTOS scheduler starts.
 */
void hmi_task_init(void);

/**
 * @brief Enter HMI mode:
 *   1. Switch GPIO46 HIGH  → UART2 routed to HMI display.
 *   2. Install UART2 BSP driver.
 *   3. Start the RX FreeRTOS task.
 *   4. Navigate display to home page.
 * @return ESP_OK on success.
 */
esp_err_t hmi_task_enter_mode(void);

/**
 * @brief Exit HMI mode:
 *   1. Stop the RX FreeRTOS task.
 *   2. Deinit UART2 BSP driver.
 *   3. Switch GPIO46 LOW   → UART2 routed back to LAN MCU.
 * @return ESP_OK on success.
 */
esp_err_t hmi_task_exit_mode(void);

/**
 * @brief Returns true when the HMI display is currently active.
 */
bool hmi_task_is_active(void);

/**
 * @brief Thread-safe status update.
 *        Can be called from any task (e.g. pwr_monitor_task).
 *        Copies @p s into an internal buffer, then calls
 *        hmi_display_refresh_status() to push the data to the display.
 * @param s  Pointer to the current system status snapshot.
 */
void hmi_task_update_status(const hmi_status_t *s);

/* ------------------------------------------------------------------ */
/*  Legacy compatibility shims                                          */
/*  Code that still uses the original API names continues to compile   */
/*  without any source-level changes.                                  */
/* ------------------------------------------------------------------ */
static inline void        hmi_handler_init(void)                   { hmi_task_init(); }
static inline esp_err_t   hmi_enter_mode(void)                     { return hmi_task_enter_mode(); }
static inline esp_err_t   hmi_exit_mode(void)                      { return hmi_task_exit_mode(); }
static inline bool        hmi_is_active(void)                      { return hmi_task_is_active(); }
static inline void        hmi_refresh_status(const hmi_status_t *s){ hmi_task_update_status(s); }

#ifdef __cplusplus
}
#endif
#endif /* HMI_TASK_H */
