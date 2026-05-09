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
#define INTERNET_MONITOR_CHECK_INTERVAL_MS  10000
/** Initial delay before first probe — allows primary to establish (ms) */
#define INTERNET_MONITOR_INITIAL_DELAY_MS   5000
/** Settle time after switching to fallback before first probe (ms)
 *  Fallback (e.g. LTE) needs time for modem startup + PPP before probing */
#define INTERNET_MONITOR_FALLBACK_SETTLE_MS 60000
/** Number of consecutive failures before switching to fallback */
#define INTERNET_MONITOR_FAIL_THRESHOLD     3
/** Consecutive successes on fallback before attempting primary restore */
#define INTERNET_MONITOR_RECOVER_THRESHOLD  6  /* 12 × 10 s = 2 min */
/** TCP probe target: Google Public DNS */
#define INTERNET_MONITOR_PROBE_HOST         "8.8.4.4"
#define INTERNET_MONITOR_PROBE_PORT         53
/** Secondary probe target: Cloudflare HTTPS -- used to confirm failure */
#define INTERNET_MONITOR_PROBE_HOST2        "1.1.1.1"
#define INTERNET_MONITOR_PROBE_PORT2        443
/** TCP connect timeout per probe attempt (s) */
#define INTERNET_MONITOR_PROBE_TIMEOUT_S    2
/** Delay given to primary to re-establish when retrying from fallback (ms) */
#define INTERNET_MONITOR_RESTORE_DELAY_MS   20000

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
