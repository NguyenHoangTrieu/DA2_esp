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
#include "i2c_dev_support.h"
#include <string.h>

static const char *TAG = "STACK_HANDLER";

/* ===== Internal State ===== */

static bool              g_initialized = false;
static SemaphoreHandle_t g_stack_mutex[STACK_HANDLER_MAX_STACKS];
static char              g_module_id[4] = "000"; /* Fixed: WAN IOX1 has no adapter ID straps */

/* ===== Helper Functions ===== */

static inline bool is_valid_stack_id(uint8_t stack_id) {
  return (stack_id < STACK_HANDLER_MAX_STACKS);
}

static inline bool is_valid_pin(stack_gpio_pin_num_t pin) {
  return ((uint8_t)pin < STACK_GPIO_PIN_COUNT);
}

/* Read 4-bit module ID from WAN adapter IOX (TCA6416A at 0x21, P00-P03).
 * This is SEPARATE from IOX1 (main board TCA6416A at 0x20).
 * The adapter board has its own TCA6416A just like LAN adapters.
 * Non-fatal: if adapter has no IOX, g_module_id stays "000".
 */
static void read_module_id(void) {
    i2c_master_dev_handle_t adapter_dev = NULL;
    esp_err_t ret = i2c_dev_support_add_device(TCA6416A_I2C_ADDR_1, TCA6416A_I2C_FREQ_HZ, &adapter_dev);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No adapter IOX at 0x21, module_id=000");
        return;
    }
    /* Probe: read Config0 register */
    uint8_t reg = TCA6416A_CONFIG_PORT0, dummy;
    ret = i2c_dev_support_write_read(adapter_dev, &reg, 1, &dummy, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Adapter IOX 0x21 not responding, module_id=000");
        i2c_dev_support_remove_device(adapter_dev);
        return;
    }
    /* Read Input Port 0 (P00-P07), mask P00-P03 for module ID */
    reg = TCA6416A_INPUT_PORT0;
    uint8_t port0_val = 0;
    ret = i2c_dev_support_write_read(adapter_dev, &reg, 1, &port0_val, 1, 100);
    i2c_dev_support_remove_device(adapter_dev);
    if (ret == ESP_OK) {
        uint8_t id = port0_val & 0x0F;
        snprintf(g_module_id, sizeof(g_module_id), "%03u", id);
        ESP_LOGI(TAG, "WAN adapter module ID: %s (P00-P03=0x%X)", g_module_id, id);
    }
}

static void get_tca_mapping(uint8_t stack_id, stack_gpio_pin_num_t pin,
                            tca_port_t *port, uint8_t *pin_num) {
  (void)stack_id; /* WAN: only one stack */
  *port    = (pin < 8) ? TCA_PORT_0 : TCA_PORT_1;
  *pin_num = (uint8_t)(pin % 8);
}

/* ===== API Implementation ===== */

esp_err_t stack_handler_init(void) {
  if (g_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  snprintf(g_module_id, sizeof(g_module_id), "000");

  ESP_LOGI(TAG, "Initializing stack handler");

  if (tca_test_connection() != ESP_OK) {
    ESP_LOGE(TAG, "TCA6416A not available");
    return ESP_ERR_INVALID_STATE;
  }

  // Configure both TCA ports as inputs by default (TCA6416A: 2 ports)
  esp_err_t ret;

  ret = tca_configure_port(TCA_PORT_0, 0xFF);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure TCA Port 0");
    return ret;
  }

  ret = tca_configure_port(TCA_PORT_1, 0xFF);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure TCA Port 1");
    return ret;
  }

  // Initialize mutexes for each stack
  for (int i = 0; i < STACK_HANDLER_MAX_STACKS; i++) {
    g_stack_mutex[i] = xSemaphoreCreateMutex();
    if (!g_stack_mutex[i]) {
      ESP_LOGE(TAG, "Failed to create mutex for stack %d", i);
      // Clean up previously created mutexes
      for (int j = 0; j < i; j++) {
        vSemaphoreDelete(g_stack_mutex[j]);
      }
      return ESP_ERR_NO_MEM;
    }
  }

  /* Read adapter module ID from P00-P03 (ID pins are inputs, sampled once at boot) */
  read_module_id();

  g_initialized = true;

  ESP_LOGI(TAG, "WAN Stack initialized (single stack, 16 pins, TCA6416A)");
  ESP_LOGI(TAG, "  P00-P07 → PORT_0 | P10-P17 → PORT_1");
  ESP_LOGI(TAG, "  Adapter module_id=%s", g_module_id);

  return ESP_OK;
}

esp_err_t stack_handler_gpio_write(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                   bool level) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id)) {
    ESP_LOGE(TAG, "Invalid stack ID: %d", stack_id);
    return ESP_ERR_INVALID_ARG;
  }

  if (!is_valid_pin(pin)) {
    ESP_LOGE(TAG, "Invalid pin: %d", pin);
    return ESP_ERR_INVALID_ARG;
  }

  tca_port_t port;
  uint8_t pin_num;
  get_tca_mapping(stack_id, pin, &port, &pin_num);

  esp_err_t ret = tca_set_pin_verified(port, pin_num, level, true);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write Stack%d GPIO%d (P%d%d)", stack_id + 1,
             pin + 1, port, pin_num);
  }

  return ret;
}

esp_err_t stack_handler_gpio_read(uint8_t stack_id, stack_gpio_pin_num_t pin,
                                  bool *level) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id) || !level) {
    ESP_LOGE(TAG, "Invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  if (!is_valid_pin(pin)) {
    ESP_LOGE(TAG, "Invalid pin: %d", pin);
    return ESP_ERR_INVALID_ARG;
  }

  tca_port_t port;
  uint8_t pin_num;
  get_tca_mapping(stack_id, pin, &port, &pin_num);

  esp_err_t ret = tca_read_pin(port, pin_num, level);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read Stack%d GPIO%d (P%d%d)", stack_id + 1,
             pin + 1, port, pin_num);
  }

  return ret;
}

esp_err_t stack_handler_gpio_set_direction(uint8_t stack_id,
                                           stack_gpio_pin_num_t pin,
                                           bool is_output) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id)) {
    ESP_LOGE(TAG, "Invalid stack ID: %d", stack_id);
    return ESP_ERR_INVALID_ARG;
  }

  if (!is_valid_pin(pin)) {
    ESP_LOGE(TAG, "Invalid pin: %d", pin);
    return ESP_ERR_INVALID_ARG;
  }

  tca_port_t port;
  uint8_t pin_num;
  get_tca_mapping(stack_id, pin, &port, &pin_num);

  // Read current CONFIGURATION register
  uint8_t port_cfg;
  esp_err_t ret = tca_read_config_register(port, &port_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read config register P%d", port);
    return ret;
  }

  ESP_LOGI(TAG, "Before: Stack%d GPIO%d (P%d.%d) config=0x%02X", stack_id + 1,
           pin + 1, port, pin_num, port_cfg);

  // Modify pin direction (TCA6416A: 1=input, 0=output)
  if (is_output) {
    port_cfg &= ~(1 << pin_num); // Clear bit = output
  } else {
    port_cfg |= (1 << pin_num); // Set bit = input
  }

  // Write back configuration
  ret = tca_configure_port(port, port_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write config P%d", port);
    return ret;
  }

  // Verify
  uint8_t verify_cfg;
  ret = tca_read_config_register(port, &verify_cfg);
  if (ret == ESP_OK) {
    bool actual_is_output = !(verify_cfg & (1 << pin_num));
    ESP_LOGI(TAG, "After: Stack%d GPIO%d (P%d.%d) config=0x%02X - %s %s",
             stack_id + 1, pin + 1, port, pin_num, verify_cfg,
             actual_is_output ? "OUTPUT" : "INPUT",
             (actual_is_output == is_output) ? "OK" : "FAILED");
  }

  return ret;
}
/* ===== New APIs for Module Controller Support ===== */

esp_err_t stack_handler_gpio_write_multi(uint8_t stack_id,
                                         const gpio_action_t *actions,
                                         size_t count) {
  if (!g_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (!is_valid_stack_id(stack_id) || !actions || count == 0) {
    ESP_LOGE(TAG, "Invalid arguments");
    return ESP_ERR_INVALID_ARG;
  }

  // Group actions by TCA port to minimize I2C transactions
  uint8_t port_masks[2]  = {0};
  uint8_t port_states[2] = {0};

  // Build port masks and states
  for (size_t i = 0; i < count; i++) {
    if (!is_valid_pin(actions[i].pin)) {
      ESP_LOGW(TAG, "Skipping invalid pin %d", actions[i].pin);
      continue;
    }

    tca_port_t port;
    uint8_t pin_num;
    get_tca_mapping(stack_id, actions[i].pin, &port, &pin_num);

    port_masks[port] |= (1 << pin_num);
    if (actions[i].level) {
      port_states[port] |= (1 << pin_num);
    }
  }

  // Write to each port once (batched operation)
  esp_err_t ret = ESP_OK;
  for (int port = 0; port < 2; port++) {
    if (port_masks[port] != 0) {
      // Step 1: Set affected pins as OUTPUT in CONFIG register (0=output, 1=input)
      uint8_t cfg;
      ret = tca_read_config_register((tca_port_t)port, &cfg);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read CONFIG P%d", port);
        break;
      }
      cfg &= ~port_masks[port]; // Clear bits = set to output
      ret = tca_configure_port((tca_port_t)port, cfg);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set OUTPUT direction P%d", port);
        break;
      }

      // Step 2: Read-modify-write OUTPUT register
      uint8_t current;
      ret = tca_read_output_register((tca_port_t)port, &current);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read P%d output register", port);
        break;
      }

      // Modify only target bits
      current = (current & ~port_masks[port]) |
                (port_states[port] & port_masks[port]);

      // Write back
      ret = tca_write_output_register((tca_port_t)port, current);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write P%d output register", port);
        break;
      }

      ESP_LOGD(TAG, "P%d: cfg=0x%02X, out=0x%02X (mask=0x%02X)",
               port, cfg, current, port_masks[port]);
    }
  }

  ESP_LOGD(TAG, "Stack%d: Batch wrote %d GPIO actions", stack_id + 1, count);
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
 * @brief Get WAN board fixed ID.
 *
 * The WAN MCU's TCA6416A (IOX1) is the main-board IO expander controlling
 * BC_IO, power rails, fan, and RGB LED — P00-P03 are NOT adapter ID straps.
 * Returns fixed "000" for compatibility with NVS change-detection logic.
 */
const char *stack_handler_get_module_id(uint8_t stack_id) {
  if (!is_valid_stack_id(stack_id)) {
    return "000";
  }
  return g_module_id;
}

