#ifndef INTERNET_MONITOR_H
#define INTERNET_MONITOR_H

#include "config_handler.h"
#include <stdbool.h>

/**
 * @brief Internet Monitor — monitors connectivity and switches to fallback
 *        when the primary connection fails.
 *
 * Fallback rules:
 *   Primary = LTE      →  fallback = WiFi
 *   Primary = ETHERNET →  fallback = WiFi
 *   Primary = WiFi     →  fallback = LTE  (if LTE APN configured)
 *                                          else ETHERNET
 *
 * Controlled by:
 *   g_internet_fallback      (bool)  — enable / disable fallback mechanism
 *   g_internet_fallback_type (enum)  — auto-set when primary type changes
 *
 * Both vars are declared in config_handler.c and saved to NVS.
 */

/** Check interval between connectivity probes (ms) */
#define INTERNET_MONITOR_CHECK_INTERVAL_MS  30000
/** Number of consecutive failures before switching to fallback */
#define INTERNET_MONITOR_FAIL_THRESHOLD     3
/** TCP probe target: Google DNS port 53 */
#define INTERNET_MONITOR_PROBE_HOST         "8.8.4.4"
#define INTERNET_MONITOR_PROBE_PORT         53
/** TCP connect timeout per probe attempt (s) */
#define INTERNET_MONITOR_PROBE_TIMEOUT_S    3

/**
 * @brief Start the internet monitor task.
 *        Must be called AFTER the primary internet connection task is started
 *        and only when g_internet_fallback == true.
 */
void internet_monitor_task_start(void);

/**
 * @brief Stop the internet monitor task.
 */
void internet_monitor_task_stop(void);

/**
 * @brief One-shot connectivity check via TCP connect to PROBE_HOST:PROBE_PORT.
 * @return true  if internet is reachable
 *         false otherwise
 */
bool internet_monitor_check_connectivity(void);

#endif /* INTERNET_MONITOR_H */
