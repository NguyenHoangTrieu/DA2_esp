/**
 * @file stack_handler.h
 * @brief WAN Communication Stack GPIO Manager - Single Stack
 *
 * WAN MCU has one stack connector with 11 GPIO + WAKE# + PERST# signals
 * controlled via TCA6424ARGJR I2C GPIO expander.
 *
 * JSON pin label convention (WAN single stack, stack_id always 0):
 *   "01" - "11"  ->  GPIO 1-11
 *   "WK"         ->  WAKE# (active-low wake signal)
 *   "PE"         ->  PERST# (active-low reset)
 */

#ifndef STACK_HANDLER_H
#define STACK_HANDLER_H

#include "esp_err.h"
#include "tca_handler.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Constants ===== */
#define STACK_HANDLER_MAX_STACKS  1     /**< WAN has exactly one stack              */
#define STACK_GPIO_PIN_COUNT      13    /**< 11 GPIO + WAKE# + PERST#               */
#define STACK_GPIO_PIN_NONE       0xFF  /**< Sentinel: no pin assigned              */

/* ===== GPIO Pin Identifiers ===== */
/**
 * @brief GPIO pin enum — also used as index into the TCA mapping table.
 *
 * Relationship to JSON label (WAN, stack_id = 0):
 *   STACK_GPIO_PIN_1  ..  STACK_GPIO_PIN_11  <->  "01" .. "11"
 *   STACK_GPIO_PIN_WAKE                      <->  "WK"
 *   STACK_GPIO_PIN_PERST                     <->  "PE"
 */
typedef enum {
  STACK_GPIO_PIN_1    = 0,
  STACK_GPIO_PIN_2    = 1,
  STACK_GPIO_PIN_3    = 2,
  STACK_GPIO_PIN_4    = 3,
  STACK_GPIO_PIN_5    = 4,
  STACK_GPIO_PIN_6    = 5,
  STACK_GPIO_PIN_7    = 6,
  STACK_GPIO_PIN_8    = 7,
  STACK_GPIO_PIN_9    = 8,
  STACK_GPIO_PIN_10   = 9,
  STACK_GPIO_PIN_11   = 10,
  STACK_GPIO_PIN_WAKE  = 11,  /**< WAKE#  – active-low wake signal  ("WK")  */
  STACK_GPIO_PIN_PERST = 12   /**< PERST# – active-low reset signal ("PE")  */
} stack_gpio_pin_num_t;

/* ===== API Functions ===== */

/* ===== GPIO action structure for batch operations ===== */
typedef struct {
  stack_gpio_pin_num_t pin;
  bool level;
} gpio_action_t;

/* ===== API ===== */

/**
 * @brief Initialize stack handler and TCA GPIO expander.
 * @return ESP_OK on success
 */
esp_err_t stack_handler_init(void);

/**
 * @brief Write a GPIO pin level.
 * @param stack_id Must be 0 (WAN has one stack).
 * @param pin      Pin identifier (STACK_GPIO_PIN_1 .. STACK_GPIO_PIN_PERST).
 * @param level    true = HIGH, false = LOW.
 */
esp_err_t stack_handler_gpio_write(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                   bool level);

/**
 * @brief Read a GPIO pin level.
 * @param stack_id Must be 0.
 * @param pin      Pin identifier.
 * @param level    Output: current level.
 */
esp_err_t stack_handler_gpio_read(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                  bool *level);

/**
 * @brief Configure GPIO pin direction.
 * @param stack_id  Must be 0.
 * @param pin       Pin identifier.
 * @param is_output true = output, false = input.
 */
esp_err_t stack_handler_gpio_set_direction(uint8_t stack_id,
                                           stack_gpio_pin_num_t pin,
                                           bool is_output);

/**
 * @brief Write multiple GPIO pins in a batched I2C transaction.
 * @param stack_id Must be 0.
 * @param actions  Array of pin/level pairs.
 * @param count    Number of entries in @p actions.
 */
esp_err_t stack_handler_gpio_write_multi(uint8_t stack_id,
                                         const gpio_action_t *actions,
                                         size_t count);

/**
 * @brief Get current state of a GPIO pin.
 * @param stack_id Must be 0.
 * @param pin      Pin identifier.
 * @param state    Output: current pin state.
 */
esp_err_t stack_handler_gpio_get_state(uint8_t stack_id,
                                       stack_gpio_pin_num_t pin, bool *state);

/**
 * @brief Take the exclusive-access mutex for the stack.
 * @param stack_id Must be 0.
 * @return ESP_OK, or ESP_ERR_TIMEOUT if not acquired within 1 s.
 */
esp_err_t stack_handler_lock(uint8_t stack_id);

/**
 * @brief Release the exclusive-access mutex.
 * @param stack_id Must be 0.
 */
esp_err_t stack_handler_unlock(uint8_t stack_id);

/**
 * @brief Get WAN module ID (pseudo implementation for NVS change detection).
 *
 * Returns "001" for stack_id = 0.
 * Used to detect a module swap between boots so stale NVS config can be
 * invalidated (same pattern as LAN module_monitor_task).
 *
 * @param stack_id Must be 0.
 * @return Null-terminated module ID string ("001").
 */
const char *stack_handler_get_module_id(uint8_t stack_id);

#ifdef __cplusplus
}
#endif

#endif // STACK_HANDLER_H
