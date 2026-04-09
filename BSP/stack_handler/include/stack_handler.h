/**
 * @file stack_handler.h
 * @brief WAN Communication Stack Manager — two IOX slots (on-board 0x20 + adapter 0x21)
 *
 * Copied from LAN MCU. Each slot has a dedicated TCA6416A. Pin mapping is flat:
 *   P00-P07 -> PORT_0 bits 0-7 (enum 0-7)
 *   P10-P17 -> PORT_1 bits 0-7 (enum 8-15)
 *
 * slot 0: on-board IOX 0x20 (power rails, RGB LED, fan, BC signals)
 * slot 1: adapter IOX 0x21 (LTE modem GPIOs)
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
#define STACK_HANDLER_MAX_STACKS 2     /**< WAN MCU: slot0=on-board(0x20), slot1=adapter(0x21) */
#define STACK_GPIO_PIN_COUNT     16    /**< Full TCA6416A 16-pin direct mapping                 */
#define STACK_GPIO_PIN_NONE      0xFF  /**< Sentinel: no pin assigned                           */

/* ===== GPIO Pin Identifiers ===== */
typedef enum {
    STACK_GPIO_PIN_00 = 0,   /* P00 */
    STACK_GPIO_PIN_01 = 1,   /* P01 */
    STACK_GPIO_PIN_02 = 2,   /* P02 */
    STACK_GPIO_PIN_03 = 3,   /* P03 */
    STACK_GPIO_PIN_04 = 4,   /* P04 */
    STACK_GPIO_PIN_05 = 5,   /* P05 */
    STACK_GPIO_PIN_06 = 6,   /* P06 */
    STACK_GPIO_PIN_07 = 7,   /* P07 */
    STACK_GPIO_PIN_10 = 8,   /* P10 */
    STACK_GPIO_PIN_11 = 9,   /* P11 */
    STACK_GPIO_PIN_12 = 10,  /* P12 */
    STACK_GPIO_PIN_13 = 11,  /* P13 */
    STACK_GPIO_PIN_14 = 12,  /* P14 */
    STACK_GPIO_PIN_15 = 13,  /* P15 */
    STACK_GPIO_PIN_16 = 14,  /* P16 */
    STACK_GPIO_PIN_17 = 15,  /* P17 */
} stack_gpio_pin_num_t;

/* ===== API Functions ===== */

esp_err_t stack_handler_init(void);

esp_err_t stack_handler_gpio_write(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                   bool level);

esp_err_t stack_handler_gpio_read(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                  bool *level);

esp_err_t stack_handler_gpio_set_direction(uint8_t stack_id,
                                           stack_gpio_pin_num_t pin,
                                           bool is_output);

/* ===== New APIs for Module Controller Support ===== */

typedef struct {
  stack_gpio_pin_num_t pin;
  bool level;
} gpio_action_t;

esp_err_t stack_handler_gpio_write_multi(uint8_t stack_id,
                                         const gpio_action_t *actions,
                                         size_t count);

esp_err_t stack_handler_gpio_get_state(uint8_t stack_id,
                                       stack_gpio_pin_num_t pin, bool *state);

esp_err_t stack_handler_lock(uint8_t stack_id);

esp_err_t stack_handler_unlock(uint8_t stack_id);

const char* stack_handler_get_module_id(uint8_t stack_id);

#ifdef __cplusplus
}
#endif

#endif // STACK_HANDLER_H
