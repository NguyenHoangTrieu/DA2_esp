/**
 * @file stack_handler.c
 * @brief WAN Communication Stack GPIO Manager Implementation (TCA6416A)
 *
 * Single stack, 16 pins: direct flat mapping.
 *   pin 0-7  → TCA PORT_0, bits 0-7  (P00-P07)
 *   pin 8-15 → TCA PORT_1, bits 0-7  (P10-P17)
 */

#include "stack_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tca_handler.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "STACK_HANDLER";

/* ===== Per-slot state ===== */
static tca6416a_inst_t   g_tca[STACK_HANDLER_MAX_STACKS];
static char              g_module_id[STACK_HANDLER_MAX_STACKS][4];
static bool              g_slot_present[STACK_HANDLER_MAX_STACKS];
static SemaphoreHandle_t g_stack_mutex[STACK_HANDLER_MAX_STACKS];
static bool              g_initialized = false;

/* Fixed address / INT assign for WAN board:
 *   slot 0: on-board IOX 0x20, INT on TCA6416A_INT_PIN (wired on main PCB)
 *   slot 1: adapter IOX 0x21, no INT pin routed to WAN MCU */
static const uint8_t k_addr[STACK_HANDLER_MAX_STACKS]    = { TCA6416A_I2C_ADDR_0,  TCA6416A_I2C_ADDR_1 };
static const int     k_int_gpio[STACK_HANDLER_MAX_STACKS] = { TCA6416A_INT_PIN, -1 };

/* ===== Helpers ===== */

static inline bool is_valid_stack_id(uint8_t id)       { return id < STACK_HANDLER_MAX_STACKS; }
static inline bool is_valid_pin(stack_gpio_pin_num_t p) { return (uint8_t)p < STACK_GPIO_PIN_COUNT; }

/* Flat mapping: enum 0-7 → PORT_0 bit 0-7, enum 8-15 → PORT_1 bit 0-7 */
static void get_tca_mapping(stack_gpio_pin_num_t pin, tca_port_t *port, uint8_t *pin_num) {
    *port    = ((uint8_t)pin < 8) ? TCA_PORT_0 : TCA_PORT_1;
    *pin_num = (uint8_t)((uint8_t)pin % 8);
}

/* Module ID:
 *   slot 0 (on-board 0x20): P00-P03 are BC_IO/system signals, NOT ID straps → always "000".
 *   slot 1 (adapter 0x21): P00-P03 are 4-bit adapter ID straps. */
static void read_module_id(uint8_t slot) {
    if (slot == 0 || !g_slot_present[slot]) {
        snprintf(g_module_id[slot], sizeof(g_module_id[slot]), "000");
        return;
    }
    uint8_t port0_val = 0;
    esp_err_t ret = tca_read_port_inst(&g_tca[slot], TCA_PORT_0, &port0_val);
    if (ret == ESP_OK) {
        snprintf(g_module_id[slot], sizeof(g_module_id[slot]), "%03u", port0_val & 0x0F);
        ESP_LOGI(TAG, "Adapter module_id=%s (P00-P03=0x%X)",
                 g_module_id[slot], port0_val & 0x0F);
    } else {
        snprintf(g_module_id[slot], sizeof(g_module_id[slot]), "000");
    }
}

/* ===== API Implementation ===== */

esp_err_t stack_handler_init(void) {
    if (g_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    memset(g_tca,          0, sizeof(g_tca));
    memset(g_slot_present, 0, sizeof(g_slot_present));
    for (int i = 0; i < STACK_HANDLER_MAX_STACKS; i++)
        snprintf(g_module_id[i], sizeof(g_module_id[i]), "000");

    for (int slot = 0; slot < STACK_HANDLER_MAX_STACKS; slot++) {
        g_stack_mutex[slot] = xSemaphoreCreateMutex();
        if (!g_stack_mutex[slot]) {
            ESP_LOGE(TAG, "Failed to create mutex for slot %d", slot);
            for (int j = 0; j < slot; j++) vSemaphoreDelete(g_stack_mutex[j]);
            return ESP_ERR_NO_MEM;
        }
    }

    /* --- Slot 0: on-board IOX 0x20 ---
     * Reuse the instance that tca_init() already registered with i2c_dev_support
     * (called in app_main before stack_handler_init). */
    {
        tca6416a_inst_t *onboard = tca_get_onboard_inst();
        if (!onboard || !onboard->initialized) {
            ESP_LOGE(TAG, "On-board IOX 0x20 not ready — call tca_init() before stack_handler_init()");
            return ESP_ERR_INVALID_STATE;
        }
        g_tca[0]         = *onboard;  /* copy handle + metadata */
        g_slot_present[0] = true;
        read_module_id(0);
        ESP_LOGI(TAG, "Slot 0 (on-board IOX 0x%02X) ready", k_addr[0]);
    }

    /* --- Slot 1: adapter IOX 0x21 --- optional, no INT pin routed */
    {
        esp_err_t ret = tca_init_inst(&g_tca[1], k_addr[1], k_int_gpio[1]);
        if (ret == ESP_OK) {
            g_slot_present[1] = true;
            read_module_id(1);
            ESP_LOGI(TAG, "Slot 1 (adapter IOX 0x%02X) ready, module_id=%s",
                     k_addr[1], g_module_id[1]);
        } else {
            ESP_LOGI(TAG, "Slot 1 (adapter IOX 0x%02X) not present: %s",
                     k_addr[1], esp_err_to_name(ret));
        }
    }

    g_initialized = true;
    ESP_LOGI(TAG, "WAN Stack: slot0=on-board(0x%02X) slot1=adapter(0x%02X %s)",
             k_addr[0], k_addr[1], g_slot_present[1] ? "PRESENT" : "ABSENT");
    ESP_LOGI(TAG, "  Adapter module_id=%s", g_module_id[1]);
    return ESP_OK;
}

esp_err_t stack_handler_gpio_write(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                   bool level) {
    if (!g_initialized)               return ESP_ERR_INVALID_STATE;
    if (!is_valid_stack_id(stack_id)) return ESP_ERR_INVALID_ARG;
    if (!g_slot_present[stack_id])    return ESP_ERR_NOT_FOUND;
    if (!is_valid_pin(pin))           return ESP_ERR_INVALID_ARG;

    tca_port_t port; uint8_t pin_num;
    get_tca_mapping(pin, &port, &pin_num);
    return tca_set_pin_verified_inst(&g_tca[stack_id], port, pin_num, level, true);
}

esp_err_t stack_handler_gpio_read(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                  bool *level) {
    if (!g_initialized)                        return ESP_ERR_INVALID_STATE;
    if (!is_valid_stack_id(stack_id) || !level) return ESP_ERR_INVALID_ARG;
    if (!g_slot_present[stack_id])             return ESP_ERR_NOT_FOUND;
    if (!is_valid_pin(pin))                    return ESP_ERR_INVALID_ARG;

    tca_port_t port; uint8_t pin_num;
    get_tca_mapping(pin, &port, &pin_num);
    return tca_read_pin_inst(&g_tca[stack_id], port, pin_num, level);
}

esp_err_t stack_handler_gpio_set_direction(uint8_t stack_id,
                                           stack_gpio_pin_num_t pin,
                                           bool is_output) {
    if (!g_initialized)               return ESP_ERR_INVALID_STATE;
    if (!is_valid_stack_id(stack_id)) return ESP_ERR_INVALID_ARG;
    if (!g_slot_present[stack_id])    return ESP_ERR_NOT_FOUND;
    if (!is_valid_pin(pin))           return ESP_ERR_INVALID_ARG;

    tca_port_t port; uint8_t pin_num;
    get_tca_mapping(pin, &port, &pin_num);

    uint8_t cfg;
    esp_err_t ret = tca_read_config_register_inst(&g_tca[stack_id], port, &cfg);
    if (ret != ESP_OK) return ret;

    if (is_output) cfg &= ~(uint8_t)(1u << pin_num);
    else           cfg |=  (uint8_t)(1u << pin_num);

    return tca_configure_port_inst(&g_tca[stack_id], port, cfg);
}
esp_err_t stack_handler_gpio_write_multi(uint8_t stack_id,
                                         const gpio_action_t *actions,
                                         size_t count) {
    if (!g_initialized)                                         return ESP_ERR_INVALID_STATE;
    if (!is_valid_stack_id(stack_id) || !actions || count == 0) return ESP_ERR_INVALID_ARG;
    if (!g_slot_present[stack_id])                              return ESP_ERR_NOT_FOUND;

    uint8_t port_masks[2]  = {0};
    uint8_t port_states[2] = {0};

    for (size_t i = 0; i < count; i++) {
        if (!is_valid_pin(actions[i].pin)) {
            ESP_LOGW(TAG, "Skipping invalid pin %d", actions[i].pin);
            continue;
        }
        tca_port_t port; uint8_t pin_num;
        get_tca_mapping(actions[i].pin, &port, &pin_num);
        port_masks[port]  |= (uint8_t)(1u << pin_num);
        if (actions[i].level) port_states[port] |= (uint8_t)(1u << pin_num);
    }

    esp_err_t ret = ESP_OK;
    for (int p = 0; p < 2 && ret == ESP_OK; p++) {
        if (port_masks[p] == 0) continue;

        uint8_t cfg;
        ret = tca_read_config_register_inst(&g_tca[stack_id], (tca_port_t)p, &cfg);
        if (ret != ESP_OK) break;
        cfg &= ~port_masks[p];
        ret = tca_configure_port_inst(&g_tca[stack_id], (tca_port_t)p, cfg);
        if (ret != ESP_OK) break;

        uint8_t current;
        ret = tca_read_output_register_inst(&g_tca[stack_id], (tca_port_t)p, &current);
        if (ret != ESP_OK) break;
        current = (current & ~port_masks[p]) | (port_states[p] & port_masks[p]);
        ret = tca_write_output_register_inst(&g_tca[stack_id], (tca_port_t)p, current);
    }

    return ret;
}

esp_err_t stack_handler_gpio_get_state(uint8_t stack_id,
                                       stack_gpio_pin_num_t pin, bool *state) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id) || !is_valid_pin(pin) || !state) {
    ESP_LOGE(TAG, "Invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  // Read current state (same as stack_handler_gpio_read)
  return stack_handler_gpio_read(stack_id, pin, state);
}

esp_err_t stack_handler_lock(uint8_t stack_id) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id)) {
    ESP_LOGE(TAG, "Invalid stack ID: %d", stack_id);
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_stack_mutex[stack_id], pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire mutex for stack%d", stack_id + 1);
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGD(TAG, "Stack%d locked", stack_id + 1);
  return ESP_OK;
}

esp_err_t stack_handler_unlock(uint8_t stack_id) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id)) {
    ESP_LOGE(TAG, "Invalid stack ID: %d", stack_id);
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreGive(g_stack_mutex[stack_id]);
  ESP_LOGD(TAG, "Stack%d unlocked", stack_id + 1);

  return ESP_OK;
}

/**
 * @brief Get WAN board module ID.
 *
 * slot 0 (on-board IOX 0x20): P00-P03 are BC_IO signals — always returns "000".
 * slot 1 (adapter IOX 0x21): returns 4-bit adapter ID strapped at P00-P03.
 */
const char *stack_handler_get_module_id(uint8_t stack_id) {
    if (!is_valid_stack_id(stack_id)) return "000";
    return g_module_id[stack_id];
}

