/**
 * @file tca_handler.h
 * @brief TCA6416A 16-bit I2C I/O Expander Handler (WAN MCU - single instance)
 *
 * Replaces TCA6424A. Key differences:
 *   - Only 2 ports (PORT_0, PORT_1) â€” no PORT_2
 *   - Different register map (OUTPUT at 0x02/0x03, CONFIG at 0x06/0x07)
 *   - I2C address: 0x20 or 0x21 (scanned at boot)
 *   - INT: GPIO47, RESET: GPIO48
 */

#ifndef TCA_HANDLER_H
#define TCA_HANDLER_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "i2c_dev_support.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// TCA6416A Configuration
// ADDR pin: GND â†’ 0x20, VCC â†’ 0x21. WAN side has one adapter; address scanned at boot.
#define TCA6416A_I2C_ADDR_0    0x20    // ADDR = GND (default)
#define TCA6416A_I2C_ADDR_1    0x21    // ADDR = VCC
#define TCA6416A_I2C_FREQ_HZ   400000  // 400kHz

// GPIO control pins (new board pinout)
#define TCA6416A_INT_PIN    21  // IO expander INT (active-low)
#define TCA6416A_RESET_PIN  38  // IO expander RESET (active-low)

// Port definitions â€” TCA6416A has 2 ports only
typedef enum {
    TCA_PORT_0 = 0,
    TCA_PORT_1 = 1,
} tca_port_t;

// TCA6416A register map
#define TCA6416A_INPUT_PORT0    0x00
#define TCA6416A_INPUT_PORT1    0x01
#define TCA6416A_OUTPUT_PORT0   0x02  // TCA6424A was 0x04
#define TCA6416A_OUTPUT_PORT1   0x03  // TCA6424A was 0x05
#define TCA6416A_POLARITY_PORT0 0x04  // TCA6424A was 0x08
#define TCA6416A_POLARITY_PORT1 0x05  // TCA6424A was 0x09
#define TCA6416A_CONFIG_PORT0   0x06  // TCA6424A was 0x0C
#define TCA6416A_CONFIG_PORT1   0x07  // TCA6424A was 0x0D

/**
 * @brief Per-instance context for one TCA6416A device.
 *
 * Allocate one of these per IOX device and pass its pointer to every _inst() API call.
 * Zero-initialise before calling tca_init_inst().
 * On WAN board: one instance for on-board IOX@0x20, one for adapter IOX@0x21.
 */
typedef struct {
    i2c_master_dev_handle_t dev_handle;
    int                     int_gpio;     /**< INT GPIO, or -1 if not connected */
    uint8_t                 i2c_addr;     /**< I2C address (0x20 or 0x21) for logging */
    bool                    initialized;
} tca6416a_inst_t;

/**
 * @brief TCA6416A interrupt callback function type
 * @param port Port number that triggered interrupt
 * @param pin_state Current state of port pins
 */
typedef void (*tca_interrupt_callback_t)(tca_port_t port, uint8_t pin_state);

/**
 * @brief Initialize TCA6416A (requires i2c_dev_support to be initialized first)
 * @return ESP_OK on success
 */
esp_err_t tca_init(void);

/**
 * @brief Initialize with a specific I2C address (for address scanning).
 * @param i2c_addr  TCA6416A_I2C_ADDR_0 (0x20) or TCA6416A_I2C_ADDR_1 (0x21).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no device at addr.
 */
esp_err_t tca_init_with_addr(uint8_t i2c_addr);

/**
 * @brief Test if TCA6416A is responding
 * @return ESP_OK if device responds
 */
esp_err_t tca_test_connection(void);

/**
 * @brief Deinitialize TCA6416A
 * @return ESP_OK on success
 */
esp_err_t tca_deinit(void);

/**
 * @brief Hardware reset TCA6416A
 * @return ESP_OK on success
 */
esp_err_t tca_reset(void);

/**
 * @brief Configure port pins as input or output
 * @param port Port number (0-1)
 * @param config Configuration byte (1=input, 0=output)
 * @return ESP_OK on success
 */
esp_err_t tca_configure_port(tca_port_t port, uint8_t config);

/**
 * @brief Read input port
 * @param port Port number (0-1)
 * @param value Pointer to store port value
 * @return ESP_OK on success
 */
esp_err_t tca_read_port(tca_port_t port, uint8_t *value);

/**
 * @brief Write output port
 * @param port Port number (0-1)
 * @param value Value to write
 * @return ESP_OK on success
 */
esp_err_t tca_write_port(tca_port_t port, uint8_t value);

/**
 * @brief Read output port current state
 * @param port Port number (0-1)
 * @param value Pointer to store value
 * @return ESP_OK on success
 */
esp_err_t tca_read_output_port(tca_port_t port, uint8_t *value);

/**
 * @brief Set polarity inversion
 * @param port Port number (0-1)
 * @param polarity Polarity byte (1=inverted, 0=normal)
 * @return ESP_OK on success
 */
esp_err_t tca_set_polarity(tca_port_t port, uint8_t polarity);

/**
 * @brief Set pin with verification
 * @param port Port number (0-1)
 * @param pin Pin number (0-7)
 * @param level Pin level (true=high, false=low)
 * @param verify Enable verification (true=verify, false=no verify)
 * @return ESP_OK on success
 */
esp_err_t tca_set_pin_verified(tca_port_t port, uint8_t pin, bool level,
                               bool verify);

/**
 * @brief Read single pin
 * @param port Port number (0-1)
 * @param pin Pin number (0-7)
 * @param level Pointer to store pin level
 * @return ESP_OK on success
 */
esp_err_t tca_read_pin(tca_port_t port, uint8_t pin, bool *level);

/**
 * @brief Register interrupt callback
 * @param callback Callback function
 * @return ESP_OK on success
 */
esp_err_t tca_register_interrupt_callback(tca_interrupt_callback_t callback);

/**
 * @brief Read configuration register of a port
 * @param port Port number (0-1)
 * @param value Output value pointer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tca_read_config_register(tca_port_t port, uint8_t *value);

/**
 * @brief Read output register of a port
 * @param port Port number (0-1)
 * @param value Output value pointer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tca_read_output_register(tca_port_t port, uint8_t *value);

/**
 * @brief Write output register of a port
 * @param port Port number (0-1)
 * @param value Value to write
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tca_write_output_register(tca_port_t port, uint8_t value);

/* ===== Multi-instance API (supports both 0x20 on-board and 0x21 adapter) ===== */

/**
 * @brief Return a pointer to the on-board IOX instance populated by tca_init().
 *
 * stack_handler calls this so it can use the same dev_handle that tca_init()
 * already registered with i2c_dev_support, avoiding a double-add.
 * Returns NULL if tca_init() has not been called yet.
 */
tca6416a_inst_t *tca_get_onboard_inst(void);

/**
 * @brief Initialise one TCA6416A instance.
 *
 * Configures INT GPIO (if int_gpio >= 0), adds the I2C device, probes it,
 * and sets all pins to input. Requires i2c_dev_support to be initialised first.
 *
 * @param inst      Caller-allocated instance (zero-init before call).
 * @param i2c_addr  TCA6416A_I2C_ADDR_0 (0x20) or TCA6416A_I2C_ADDR_1 (0x21).
 * @param int_gpio  GPIO number for INT, or -1 if no INT pin is routed.
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if no device at addr.
 */
esp_err_t tca_init_inst(tca6416a_inst_t *inst, uint8_t i2c_addr, int int_gpio);

/** @brief Deinitialise an instance and release the I2C device handle. */
esp_err_t tca_deinit_inst(tca6416a_inst_t *inst);

/** @brief Probe — returns ESP_OK if the device ACKs. */
esp_err_t tca_test_connection_inst(tca6416a_inst_t *inst);

/** @brief Configure port direction (bit=1 → input, bit=0 → output). */
esp_err_t tca_configure_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t config);

/** @brief Read input register of a port. */
esp_err_t tca_read_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t *value);

/** @brief Write output register of a port. */
esp_err_t tca_write_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t value);

/** @brief Read-modify-write a single output pin with optional read-back verify. */
esp_err_t tca_set_pin_verified_inst(tca6416a_inst_t *inst, tca_port_t port,
                                    uint8_t pin, bool level, bool verify);

/** @brief Read a single input pin. */
esp_err_t tca_read_pin_inst(tca6416a_inst_t *inst, tca_port_t port,
                            uint8_t pin, bool *level);

/** @brief Read configuration (direction) register. */
esp_err_t tca_read_config_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                        uint8_t *value);

/** @brief Read output register (not input register). */
esp_err_t tca_read_output_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                        uint8_t *value);

/** @brief Write output register. */
esp_err_t tca_write_output_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                         uint8_t value);

/**
 * @brief Probe an I2C address and read P17 (IOX_SLOTDET) without full init.
 *
 * Temporarily adds the device, checks for ACK, reads INPUT_PORT1 for P17, then
 * removes the device. Used to identify which physical slot an adapter occupies.
 *
 * @param i2c_addr  Address to probe.
 * @param slotdet   Output: P17 value — 0 = slot 0, 1 = slot 1.
 * @return ESP_OK if present; ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t tca_probe_slotdet(uint8_t i2c_addr, bool *slotdet);

#ifdef __cplusplus
}
#endif

#endif /* TCA_HANDLER_H */
