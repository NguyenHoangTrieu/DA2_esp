/**
 * @file pwr_source_handler.c
 * @brief Power Source Control Handler Implementation
 */

#include "pwr_source_handler.h"
#include "esp_log.h"
#include "tca_handler.h"

static const char *TAG = "PWR_SOURCE";

esp_err_t pwr_source_init(void) {
  ESP_LOGI(TAG, "Initializing power source control");

  // Configure P1_5, P1_6, P1_7 as outputs (bit=0)
  // Other pins remain as inputs (bit=1)
  uint8_t port1_config = 0xFF; // Default all input
  port1_config &=
      ~((1 << PWR_1V8_PIN) | (1 << PWR_3V3_PIN) | (1 << PWR_5V0_PIN));

  esp_err_t ret = tca_configure_port(TCA_PORT_1, port1_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure port 1: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "Port 1 configured: 0x%02X (0=output, 1=input)", port1_config);

  // Disable all power sources by default
  // ret = pwr_source_disable_all();
  // if (ret != ESP_OK) {
  //   ESP_LOGE(TAG, "Failed to disable power sources: %s", esp_err_to_name(ret));
  //   return ret;
  // }

  ESP_LOGI(
      TAG,
      "Power source control initialized (1.8V=P1_%d, 3.3V=P1_%d, 5.0V=P1_%d)",
      PWR_1V8_PIN, PWR_3V3_PIN, PWR_5V0_PIN);
  return ESP_OK;
}

esp_err_t pwr_source_set_1v8(bool enable) {
  esp_err_t ret = tca_set_pin_verified(TCA_PORT_1, PWR_3V3_PIN, enable, true);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "1.8V power rail %s", enable ? "ENABLED" : "DISABLED");
  } else {
    ESP_LOGE(TAG, "Failed to set 1.8V: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t pwr_source_set_3v3(bool enable) {
  esp_err_t ret = tca_set_pin_verified(TCA_PORT_1, PWR_3V3_PIN, enable, true);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "3.3V power rail %s", enable ? "ENABLED" : "DISABLED");
  } else {
    ESP_LOGE(TAG, "Failed to set 3.3V: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t pwr_source_set_5v0(bool enable) {
  esp_err_t ret = tca_set_pin_verified(TCA_PORT_1, PWR_5V0_PIN, enable, true);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "5.0V power rail %s", enable ? "ENABLED" : "DISABLED");
  } else {
    ESP_LOGE(TAG, "Failed to set 5.0V: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t pwr_source_get_1v8(bool *state) {
  if (!state) {
    return ESP_ERR_INVALID_ARG;
  }

  // Read OUTPUT register (not input) to get current output state
  uint8_t port_val;
  esp_err_t ret = tca_read_output_port(TCA_PORT_1, &port_val);
  if (ret == ESP_OK) {
    *state = (port_val & (1 << PWR_1V8_PIN)) != 0;
  }
  return ret;
}

esp_err_t pwr_source_get_3v3(bool *state) {
  if (!state) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t port_val;
  esp_err_t ret = tca_read_output_port(TCA_PORT_1, &port_val);
  if (ret == ESP_OK) {
    *state = (port_val & (1 << PWR_3V3_PIN)) != 0;
  }
  return ret;
}

esp_err_t pwr_source_get_5v0(bool *state) {
  if (!state) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t port_val;
  esp_err_t ret = tca_read_output_port(TCA_PORT_1, &port_val);
  if (ret == ESP_OK) {
    *state = (port_val & (1 << PWR_5V0_PIN)) != 0;
  }
  return ret;
}

esp_err_t pwr_source_disable_all(void) {
  ESP_LOGI(TAG, "Disabling all power rails");

  esp_err_t ret1 = tca_set_pin_verified(TCA_PORT_1, PWR_1V8_PIN, false, true);
  esp_err_t ret2 = tca_set_pin_verified(TCA_PORT_1, PWR_3V3_PIN, false, true);
  esp_err_t ret3 = tca_set_pin_verified(TCA_PORT_1, PWR_5V0_PIN, false, true);

  if (ret1 != ESP_OK || ret2 != ESP_OK || ret3 != ESP_OK) {
    ESP_LOGE(TAG, "Failed to disable all power sources");
    return ESP_FAIL;
  }

  return ESP_OK;
}
