/**
 * @file tca_handler.c
 * @brief TCA6416A I/O Expander Handler Implementation (WAN MCU)
 */

#include "tca_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_dev_support.h"

static const char *TAG = "TCA6416A";

static i2c_master_dev_handle_t tca_handle = NULL;
static tca_interrupt_callback_t interrupt_callback = NULL;
static uint8_t g_i2c_addr = TCA6416A_I2C_ADDR_0; /* address found at boot */
static tca6416a_inst_t g_onboard_inst = {0};      /* copy populated by tca_init() */

static esp_err_t tca_write_register(uint8_t reg, uint8_t value) {
  uint8_t data[2] = {reg, value};
  return i2c_dev_support_write(tca_handle, data, 2, 1000);
}

static esp_err_t tca_read_register(uint8_t reg, uint8_t *value) {
  return i2c_dev_support_write_read(tca_handle, &reg, 1, value, 1, 1000);
}

static void IRAM_ATTR tca_interrupt_handler(void *arg) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  // Notify task to read port status
  xHigherPriorityTaskWoken = pdTRUE;
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

esp_err_t tca_test_connection(void) {
  if (!tca_handle) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t test_val;
  esp_err_t ret = tca_read_register(TCA6416A_CONFIG_PORT0, &test_val);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "TCA6416A responding OK - Config0: 0x%02X", test_val);
  } else {
    ESP_LOGE(TAG, "TCA6416A not responding: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t tca_init(void) {
  if (!i2c_dev_support_is_initialized()) {
    ESP_LOGE(TAG, "I2C not initialized. Call i2c_dev_support_init() first");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Initializing TCA6416A IOX1 at fixed address 0x%02X", TCA6416A_I2C_ADDR_0);

  // Configure RESET GPIO
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << TCA6416A_RESET_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);

  io_conf.pin_bit_mask = (1ULL << TCA6416A_INT_PIN);
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  gpio_config(&io_conf);

  esp_err_t isr_ret = gpio_install_isr_service(0);
  if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "gpio_install_isr_service: %s (non-fatal)", esp_err_to_name(isr_ret));
  }
  gpio_isr_handler_add(TCA6416A_INT_PIN, tca_interrupt_handler, NULL);

  /* IOX1 is always at 0x20 (ADDR=GND, fixed on WAN main board schematic) */
  esp_err_t ret = i2c_dev_support_add_device(TCA6416A_I2C_ADDR_0, TCA6416A_I2C_FREQ_HZ, &tca_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add IOX1 device 0x20: %s", esp_err_to_name(ret));
    return ret;
  }
  g_i2c_addr = TCA6416A_I2C_ADDR_0;
  ret = tca_test_connection();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "IOX1 TCA6416A at 0x20 not responding");
    i2c_dev_support_remove_device(tca_handle);
    tca_handle = NULL;
    return ret;
  }

  // Configure both ports as inputs by default (TCA6416A: 2 ports only)
  tca_configure_port(TCA_PORT_0, 0xFF);
  tca_configure_port(TCA_PORT_1, 0xFF);

  ESP_LOGI(TAG, "TCA6416A initialized at 0x%02X (INT=%d, RESET=%d)",
           g_i2c_addr, TCA6416A_INT_PIN, TCA6416A_RESET_PIN);

  /* Populate the exported instance so stack_handler can use tca_*_inst() on it */
  g_onboard_inst.dev_handle  = tca_handle;
  g_onboard_inst.int_gpio    = TCA6416A_INT_PIN;
  g_onboard_inst.i2c_addr    = TCA6416A_I2C_ADDR_0;
  g_onboard_inst.initialized = true;

  return ESP_OK;
}

esp_err_t tca_init_with_addr(uint8_t i2c_addr) {
  if (!i2c_dev_support_is_initialized()) {
    ESP_LOGE(TAG, "I2C not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  if (tca_handle) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }
  esp_err_t ret = i2c_dev_support_add_device(i2c_addr, TCA6416A_I2C_FREQ_HZ, &tca_handle);
  if (ret != ESP_OK) return ret;
  g_i2c_addr = i2c_addr;
  tca_reset();
  ret = tca_test_connection();
  if (ret != ESP_OK) {
    i2c_dev_support_remove_device(tca_handle);
    tca_handle = NULL;
    return ret;
  }
  tca_configure_port(TCA_PORT_0, 0xFF);
  tca_configure_port(TCA_PORT_1, 0xFF);
  ESP_LOGI(TAG, "TCA6416A initialized at 0x%02X", i2c_addr);
  return ESP_OK;
}

esp_err_t tca_deinit(void) {
  if (tca_handle) {
    gpio_isr_handler_remove(TCA6416A_INT_PIN);
    esp_err_t ret = i2c_dev_support_remove_device(tca_handle);
    tca_handle = NULL;
    ESP_LOGI(TAG, "TCA6416A deinitialized");
    return ret;
  }
  return ESP_OK;
}

esp_err_t tca_reset(void) {
  ESP_LOGI(TAG, "Performing hardware reset");
  gpio_set_level(TCA6416A_RESET_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(TCA6416A_RESET_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(TCA6416A_RESET_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  return ESP_OK;
}

esp_err_t tca_configure_port(tca_port_t port, uint8_t config) {
  if (!tca_handle || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_CONFIG_PORT0 + port;
  esp_err_t ret = tca_write_register(reg, config);

  if (ret == ESP_OK) {
    ESP_LOGD(TAG, "Port %d configured: 0x%02X (0=output, 1=input)", port,
             config);
  } else {
    ESP_LOGE(TAG, "Failed to configure port %d: %s", port,
             esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t tca_read_port(tca_port_t port, uint8_t *value) {
  if (!tca_handle || port > TCA_PORT_1 || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_INPUT_PORT0 + port;
  return tca_read_register(reg, value);
}

esp_err_t tca_write_port(tca_port_t port, uint8_t value) {
  if (!tca_handle || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
  esp_err_t ret = tca_write_register(reg, value);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write port %d: %s", port, esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t tca_read_output_port(tca_port_t port, uint8_t *value) {
  if (!tca_handle || port > TCA_PORT_1 || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
  return tca_read_register(reg, value);
}

esp_err_t tca_set_polarity(tca_port_t port, uint8_t polarity) {
  if (!tca_handle || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_POLARITY_PORT0 + port;
  return tca_write_register(reg, polarity);
}

esp_err_t tca_set_pin_verified(tca_port_t port, uint8_t pin, bool level,
                               bool verify) {
  if (pin > 7 || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }

  // Read OUTPUT register
  uint8_t port_value;
  esp_err_t ret = tca_read_output_port(port, &port_value);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read output port %d: %s", port,
             esp_err_to_name(ret));
    return ret;
  }

  // Modify bit
  if (level) {
    port_value |= (1 << pin);
  } else {
    port_value &= ~(1 << pin);
  }

  // Write back
  ret = tca_write_port(port, port_value);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write port %d: %s", port, esp_err_to_name(ret));
    return ret;
  }

  // Verify if requested
  if (verify) {
    vTaskDelay(pdMS_TO_TICKS(1)); // Small delay for chip to update

    uint8_t verify_value;
    ret = tca_read_output_port(port, &verify_value);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to verify port %d: %s", port, esp_err_to_name(ret));
      return ret;
    }

    bool actual_state = (verify_value & (1 << pin)) != 0;
    if (actual_state == level) {
      ESP_LOGI(TAG, "[0x%02X] P%d_%d=%d VERIFIED (reg=0x%02X)",
               g_i2c_addr, port, pin, level, verify_value);
    } else {
      ESP_LOGE(TAG, "[0x%02X] P%d_%d VERIFY FAILED: expected %d got %d (reg=0x%02X)",
               g_i2c_addr, port, pin, level, actual_state, verify_value);
      return ESP_ERR_INVALID_RESPONSE;
    }
  } else {
    ESP_LOGD(TAG, "Pin P%d_%d set to %d (no verify)", port, pin, level);
  }

  return ESP_OK;
}

esp_err_t tca_read_pin(tca_port_t port, uint8_t pin, bool *level) {
  if (pin > 7 || !level || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t port_value;
  esp_err_t ret = tca_read_port(port, &port_value);

  if (ret == ESP_OK) {
    *level = (port_value & (1 << pin)) != 0;
  }

  return ret;
}

esp_err_t tca_register_interrupt_callback(tca_interrupt_callback_t callback) {
  interrupt_callback = callback;
  return ESP_OK;
}

esp_err_t tca_read_config_register(tca_port_t port, uint8_t *value) {
  if (!tca_handle || port > TCA_PORT_1 || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_CONFIG_PORT0 + port;
  return tca_read_register(reg, value);
}

esp_err_t tca_read_output_register(tca_port_t port, uint8_t *value) {
  if (!tca_handle || port > TCA_PORT_1 || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
  return tca_read_register(reg, value);
}

esp_err_t tca_write_output_register(tca_port_t port, uint8_t value) {
  if (!tca_handle || port > TCA_PORT_1) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
  return tca_write_register(reg, value);
}

/* ===================================================================== */
/*  Multi-instance API                                                   */
/* ===================================================================== */

tca6416a_inst_t *tca_get_onboard_inst(void)
{
    return (g_onboard_inst.initialized) ? &g_onboard_inst : NULL;
}

/* ISR handler for inst-based GPIO INT (no-op: polling is used instead) */
static void IRAM_ATTR tca_isr_handler_inst(void *arg) {
    (void)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

esp_err_t tca_init_inst(tca6416a_inst_t *inst, uint8_t i2c_addr, int int_gpio)
{
    if (!inst) return ESP_ERR_INVALID_ARG;
    if (!i2c_dev_support_is_initialized()) {
        ESP_LOGE(TAG, "I2C not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing TCA6416A at 0x%02X (INT=%s)", i2c_addr,
             int_gpio >= 0 ? "configured" : "none");

    inst->int_gpio    = int_gpio;
    inst->initialized = false;
    inst->dev_handle  = NULL;

    /* Configure INT GPIO if wired */
    if (int_gpio >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << int_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io_conf);
        esp_err_t isr_ret = gpio_install_isr_service(0);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "gpio_install_isr_service: %s (non-fatal)", esp_err_to_name(isr_ret));
        gpio_isr_handler_add((gpio_num_t)int_gpio, tca_isr_handler_inst, inst);
    }

    /* Add device to I2C bus */
    esp_err_t ret = i2c_dev_support_add_device(i2c_addr, TCA6416A_I2C_FREQ_HZ,
                                               &inst->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device 0x%02X: %s", i2c_addr, esp_err_to_name(ret));
        return ret;
    }

    /* Probe */
    ret = tca_test_connection_inst(inst);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No TCA6416A at 0x%02X", i2c_addr);
        i2c_dev_support_remove_device(inst->dev_handle);
        inst->dev_handle = NULL;
        return ret;
    }

    /* Default: all pins input */
    tca_configure_port_inst(inst, TCA_PORT_0, 0xFF);
    tca_configure_port_inst(inst, TCA_PORT_1, 0xFF);

    inst->i2c_addr    = i2c_addr;
    inst->initialized = true;
    ESP_LOGI(TAG, "TCA6416A 0x%02X ready", i2c_addr);
    return ESP_OK;
}

esp_err_t tca_deinit_inst(tca6416a_inst_t *inst)
{
    if (!inst || !inst->initialized) return ESP_OK;
    if (inst->int_gpio >= 0)
        gpio_isr_handler_remove((gpio_num_t)inst->int_gpio);
    esp_err_t ret = i2c_dev_support_remove_device(inst->dev_handle);
    inst->dev_handle  = NULL;
    inst->initialized = false;
    return ret;
}

esp_err_t tca_test_connection_inst(tca6416a_inst_t *inst)
{
    if (!inst || !inst->dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t val;
    uint8_t reg = TCA6416A_CONFIG_PORT0;
    esp_err_t ret = i2c_dev_support_write_read(inst->dev_handle, &reg, 1, &val, 1, 1000);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "TCA6416A OK at inst - Config0: 0x%02X", val);
    else
        ESP_LOGE(TAG, "TCA6416A inst not responding: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t tca_configure_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t config)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = { (uint8_t)(TCA6416A_CONFIG_PORT0 + port), config };
    return i2c_dev_support_write(inst->dev_handle, buf, 2, 1000);
}

esp_err_t tca_read_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t *value)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1 || !value) return ESP_ERR_INVALID_ARG;
    uint8_t reg = TCA6416A_INPUT_PORT0 + port;
    return i2c_dev_support_write_read(inst->dev_handle, &reg, 1, value, 1, 1000);
}

esp_err_t tca_write_port_inst(tca6416a_inst_t *inst, tca_port_t port, uint8_t value)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = { (uint8_t)(TCA6416A_OUTPUT_PORT0 + port), value };
    return i2c_dev_support_write(inst->dev_handle, buf, 2, 1000);
}

esp_err_t tca_set_pin_verified_inst(tca6416a_inst_t *inst, tca_port_t port,
                                    uint8_t pin, bool level, bool verify)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1 || pin > 7)
        return ESP_ERR_INVALID_ARG;

    uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
    uint8_t current;
    esp_err_t ret = i2c_dev_support_write_read(inst->dev_handle, &reg, 1, &current, 1, 1000);
    if (ret != ESP_OK) return ret;

    if (level) current |=  (uint8_t)(1u << pin);
    else       current &= ~(uint8_t)(1u << pin);

    uint8_t buf[2] = { reg, current };
    ret = i2c_dev_support_write(inst->dev_handle, buf, 2, 1000);
    if (ret != ESP_OK) return ret;

    if (verify) {
        vTaskDelay(pdMS_TO_TICKS(1));
        uint8_t readback;
        ret = i2c_dev_support_write_read(inst->dev_handle, &reg, 1, &readback, 1, 1000);
        if (ret != ESP_OK) return ret;
        bool got = (readback & (1u << pin)) != 0;
        if (got == level) {
            ESP_LOGI(TAG, "[0x%02X] P%d_%d=%d VERIFIED (reg=0x%02X)",
                     inst->i2c_addr, port, pin, level, readback);
        } else {
            ESP_LOGE(TAG, "[0x%02X] P%d_%d VERIFY FAILED: expected %d got %d (reg=0x%02X)",
                     inst->i2c_addr, port, pin, level, (int)got, readback);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

esp_err_t tca_read_pin_inst(tca6416a_inst_t *inst, tca_port_t port,
                            uint8_t pin, bool *level)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1 || pin > 7 || !level)
        return ESP_ERR_INVALID_ARG;
    uint8_t val;
    uint8_t reg = TCA6416A_INPUT_PORT0 + port;
    esp_err_t ret = i2c_dev_support_write_read(inst->dev_handle, &reg, 1, &val, 1, 1000);
    if (ret == ESP_OK) *level = (val & (1u << pin)) != 0;
    return ret;
}

esp_err_t tca_read_config_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                        uint8_t *value)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1 || !value) return ESP_ERR_INVALID_ARG;
    uint8_t reg = TCA6416A_CONFIG_PORT0 + port;
    return i2c_dev_support_write_read(inst->dev_handle, &reg, 1, value, 1, 1000);
}

esp_err_t tca_read_output_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                        uint8_t *value)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1 || !value) return ESP_ERR_INVALID_ARG;
    uint8_t reg = TCA6416A_OUTPUT_PORT0 + port;
    return i2c_dev_support_write_read(inst->dev_handle, &reg, 1, value, 1, 1000);
}

esp_err_t tca_write_output_register_inst(tca6416a_inst_t *inst, tca_port_t port,
                                         uint8_t value)
{
    if (!inst || !inst->dev_handle || port > TCA_PORT_1) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = { (uint8_t)(TCA6416A_OUTPUT_PORT0 + port), value };
    return i2c_dev_support_write(inst->dev_handle, buf, 2, 1000);
}

esp_err_t tca_probe_slotdet(uint8_t i2c_addr, bool *slotdet)
{
    if (!slotdet) return ESP_ERR_INVALID_ARG;
    if (!i2c_dev_support_is_initialized()) return ESP_ERR_INVALID_STATE;

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t ret = i2c_dev_support_add_device(i2c_addr, TCA6416A_I2C_FREQ_HZ, &handle);
    if (ret != ESP_OK) return ret;

    /* Probe: read CONFIG_PORT0 — NACK if no device */
    uint8_t dummy;
    uint8_t reg = TCA6416A_CONFIG_PORT0;
    ret = i2c_dev_support_write_read(handle, &reg, 1, &dummy, 1, 100);
    if (ret != ESP_OK) {
        i2c_dev_support_remove_device(handle);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read INPUT_PORT1 — P17 (bit 7) is IOX_SLOTDET */
    uint8_t port1_val = 0;
    reg = TCA6416A_INPUT_PORT1;
    ret = i2c_dev_support_write_read(handle, &reg, 1, &port1_val, 1, 100);
    if (ret == ESP_OK)
        *slotdet = (port1_val >> 7) & 0x01;

    i2c_dev_support_remove_device(handle);
    return ret;
}