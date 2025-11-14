#ifndef LTE_CONNECT_H
#define LTE_CONNECT_H

#include "esp_err.h"
#include "lte_handler.h"
#include <stdbool.h>

/**
 * @brief Start LTE connection task
 * 
 * This function will:
 * - Initialize LTE handler with hardcoded config
 * - Connect to network automatically
 * - Start monitoring task
 */
void lte_connect_task_start(void);

/**
 * @brief Stop LTE connection task
 */
void lte_connect_task_stop(void);

#endif /* LTE_CONNECT_H */
