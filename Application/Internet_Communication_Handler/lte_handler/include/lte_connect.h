#ifndef LTE_CONNECT_H
#define LTE_CONNECT_H

#include "esp_err.h"
#include "lte_handler.h"
#include <stdbool.h>



/* ==================== LTE Config Context ==================== */
typedef struct {
  bool initialized;
  bool task_running;
  TaskHandle_t task_handle;
  char modem_name[32];          /**< Modem model name, e.g. "A7600C1"              */
  char apn[64];                 /**< APN; empty string = LTE task will not start   */
  char username[32];
  char password[32];
  uint32_t max_reconnect_attempts;
  uint32_t reconnect_timeout_ms;
  bool auto_reconnect;
  lte_handler_comm_type_t comm_type;
  uint8_t pwr_pin;  /**< TCA pin for modem POWER (STACK_GPIO_PIN_WAKE  = 11 default) */
  uint8_t rst_pin;  /**< TCA pin for modem RESET (STACK_GPIO_PIN_PERST = 12 default) */
} lte_config_context_t;

extern lte_config_context_t g_lte_ctx;

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
