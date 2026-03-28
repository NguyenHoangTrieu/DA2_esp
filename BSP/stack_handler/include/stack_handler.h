/**
 * @file stack_handler.h
 * @brief WAN Communication Stack GPIO Manager - Single Stack (TCA6416A)
 *
 * WAN MCU has one stack connector with 16 GPIO signals (P00-P07, P10-P17)
 * controlled via TCA6416A I2C GPIO expander on the adapter board.
 *
 * Flat GPIO pin mapping (numeric pin IDs):
 *   00-07  →  P00-P07 (TCA PORT_0, pins 0-7)
 *   10-17  →  P10-P17 (TCA PORT_1, pins 0-7)
 *
 * Special pins (by function, not hardware):
 *   P00-P03  : 4-bit adapter ID (input, factory-set)
 *   P05, P06 : Typically used for modem power/reset (configurable in JSON/config)
 *   P17      : IOX_SLOTDET (input)
 *
 * Note: All pins are flat-mapped with no predefined purposes. Modem power/reset
 * pins are configured in LTE config, not hardcoded.
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
#define STACK_GPIO_PIN_COUNT      16    /**< TCA6416A 16-pin direct mapping          */
#define STACK_GPIO_PIN_NONE       0xFF  /**< Sentinel: no pin assigned              */

/* ===== GPIO Pin Identifiers ===== */
/**
 * @brief GPIO pin enum — index 0-15 maps directly to TCA6416A P00-P07, P10-P17.
 *
 * Mapping: enum value 0-7 → PORT_0 pins 0-7 (P00-P07)
 *          enum value 8-15 → PORT_1 pins 0-7 (P10-P17)
 *
 * JSON label: "00"-"07" for P00-P07, "10"-"17" for P10-P17
 */
typedef enum {
  STACK_GPIO_PIN_00 = 0,   /* P00 — adapter ID bit 0 (input)   */
  STACK_GPIO_PIN_01 = 1,   /* P01 — adapter ID bit 1 (input)   */
  STACK_GPIO_PIN_02 = 2,   /* P02 — adapter ID bit 2 (input)   */
  STACK_GPIO_PIN_03 = 3,   /* P03 — adapter ID bit 3 (input)   */
  STACK_GPIO_PIN_04 = 4,   /* P04                               */
  STACK_GPIO_PIN_05 = 5,   /* P05 (typically modem power)       */
  STACK_GPIO_PIN_06 = 6,   /* P06 (typically modem reset)       */
  STACK_GPIO_PIN_07 = 7,   /* P07                               */
  STACK_GPIO_PIN_10 = 8,   /* P10                               */
  STACK_GPIO_PIN_11 = 9,   /* P11                               */
  STACK_GPIO_PIN_12 = 10,  /* P12                               */
  STACK_GPIO_PIN_13 = 11,  /* P13                               */
  STACK_GPIO_PIN_14 = 12,  /* P14                               */
  STACK_GPIO_PIN_15 = 13,  /* P15                               */
  STACK_GPIO_PIN_16 = 14,  /* P16                               */
  STACK_GPIO_PIN_17 = 15,  /* P17 — IOX_SLOTDET (input)        */
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
 * @param pin      Pin identifier (STACK_GPIO_PIN_04 .. STACK_GPIO_PIN_17).
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
