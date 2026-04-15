#include "config_handler.h"
#include "fota_handler.h"
#include "esp_log.h"
#include "lte_connect.h"
#include "mqtt_handler.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_connect.h"
#include "stack_handler.h"
#include <string.h>

static const char *TAG = "CONFIG_NVS";

/* NVS Namespace */
#define NVS_NAMESPACE "gateway_cfg"

/* NVS Keys */
#define NVS_KEY_INTERNET_TYPE "inet_type"
#define NVS_KEY_SERVER_TYPE "srv_type"
#define NVS_KEY_WIFI_CONFIG "wifi_cfg"
#define NVS_KEY_LTE_CONFIG "lte_cfg"
#define NVS_KEY_MQTT_CONFIG "mqtt_cfg"
#define NVS_KEY_HTTP_CONFIG "http_cfg"
#define NVS_KEY_COAP_CONFIG "coap_cfg"
#define NVS_NS_GATEWAY "gateway_cfg"
#define NVS_KEY_INITIALIZED "initialized"
#define NVS_KEY_WAN_STACK_ID "wan_stack_id"  /* tracks WAN hardware stack, clears LTE config on change */
#define NVS_KEY_FOTA_WAN_URL "fota_wan_url"  /* WAN MCU firmware OTA URL */

/* WAN stack module ID — pseudo hardware identifier (always "001" for single-stack WAN) */
char g_stack_id_wan[8] = "000";

/* Default values for MQTT */
#define MQTT_DEFAULT_BROKER       "mqtt://demo.thingsboard.io:1883"
#define MQTT_DEFAULT_TOKEN        "38kozd1weulcnl6ytz8f"
#define MQTT_DEFAULT_PUB_TOPIC    "v1/devices/me/telemetry"
#define MQTT_DEFAULT_SUB_TOPIC    "v1/devices/me/rpc/request/+"
#define MQTT_DEFAULT_ATTR_TOPIC   "v1/devices/me/attributes"
#define MQTT_DEFAULT_KEEPALIVE_S  120
#define MQTT_DEFAULT_TIMEOUT_MS   10000

/* Default values for HTTP/CoAP */
#define HTTP_DEFAULT_URL        "http://192.168.1.100:8080/api/v1/{token}/telemetry"
#define HTTP_DEFAULT_TOKEN      "Zfdvk6M9rEmw5fBj7TzP"
#define HTTP_DEFAULT_PORT       8080
#define HTTP_DEFAULT_TIMEOUT_MS 10000

#define COAP_DEFAULT_HOST       "192.168.1.100"
#define COAP_DEFAULT_RESOURCE   "/api/v1/{token}/telemetry"
#define COAP_DEFAULT_TOKEN      "Zfdvk6M9rEmw5fBj7TzP"
#define COAP_DEFAULT_PORT       5683
#define COAP_DEFAULT_ACK_TO_MS  2000
#define COAP_DEFAULT_MAX_RTX    4
#define COAP_DEFAULT_RPC_POLL_MS 1500

/* External global variables from your modules */
extern wifi_config_context_t g_wifi_ctx;
extern lte_config_context_t g_lte_ctx;
extern mqtt_config_context_t g_mqtt_ctx;
extern config_internet_type_t g_internet_type;
extern config_server_type_t g_server_type;
extern http_config_data_t g_http_cfg;
extern coap_config_data_t g_coap_cfg;

/**
 * @brief Open NVS handle
 */
static esp_err_t nvs_open_handle(nvs_handle_t *handle) {
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
  }
  return err;
}

/**
 * @brief Load internet type configuration from NVS
 */
static esp_err_t load_internet_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading internet type config from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Read internet type (stored as uint8_t)
  uint8_t inet_type = CONFIG_INTERNET_WIFI;
  err = nvs_get_u8(nvs_handle, NVS_KEY_INTERNET_TYPE, &inet_type);

  if (err == ESP_OK) {
    if (inet_type < CONFIG_INTERNET_COUNT) {
      g_internet_type = (config_internet_type_t)inet_type;
      ESP_LOGI(TAG, "Internet type loaded: %d", g_internet_type);
    } else {
      ESP_LOGW(TAG, "Invalid internet type in NVS: %d", inet_type);
      err = ESP_ERR_INVALID_ARG;
    }
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "Internet type not found in NVS, using default");
    err = ESP_OK; // Not an error, use default
  } else {
    ESP_LOGE(TAG, "Error reading internet type: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save internet type configuration to NVS
 */
esp_err_t save_internet_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving internet type config to NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Write internet type
  err = nvs_set_u8(nvs_handle, NVS_KEY_INTERNET_TYPE, (uint8_t)g_internet_type);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing internet type: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Internet type saved: %d", g_internet_type);
  }

  nvs_close(nvs_handle);
  return err;
}


/**
 * @brief Load server type configuration from NVS
 */
static esp_err_t load_server_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading server type config from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Read server type (stored as uint8_t)
  uint8_t srv_type = CONFIG_SERVERTYPE_MQTT;
  err = nvs_get_u8(nvs_handle, NVS_KEY_SERVER_TYPE, &srv_type);

  if (err == ESP_OK) {
    if (srv_type < CONFIG_SERVERTYPE_COUNT) {
      g_server_type = (config_server_type_t)srv_type;
      ESP_LOGI(TAG, "Server type loaded: %d", g_server_type);
    } else {
      ESP_LOGW(TAG, "Invalid server type in NVS: %d", srv_type);
      err = ESP_ERR_INVALID_ARG;
    }
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "Server type not found in NVS, using default");
    err = ESP_OK; // Not an error, use default
  } else {
    ESP_LOGE(TAG, "Error reading server type: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save server type configuration to NVS
 */
esp_err_t save_server_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving server type config to NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Write server type
  err = nvs_set_u8(nvs_handle, NVS_KEY_SERVER_TYPE, (uint8_t)g_server_type);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing server type: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Server type saved: %d", g_server_type);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Load WiFi configuration from NVS
 */
static esp_err_t load_wifi_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading WiFi config from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Read WiFi config blob
  size_t required_size = sizeof(wifi_config_context_t);
  err = nvs_get_blob(nvs_handle, NVS_KEY_WIFI_CONFIG, &g_wifi_ctx,
                     &required_size);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "WiFi config loaded - SSID: %s", g_wifi_ctx.ssid);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "WiFi config not found in NVS, using defaults");
    err = ESP_OK; // Not an error, use defaults
  } else {
    ESP_LOGE(TAG, "Error reading WiFi config: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save WiFi configuration to NVS
 */
esp_err_t save_wifi_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving WiFi config to NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Write WiFi config blob
  err = nvs_set_blob(nvs_handle, NVS_KEY_WIFI_CONFIG, &g_wifi_ctx,
                     sizeof(wifi_config_context_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing WiFi config: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "WiFi config saved - SSID: %s", g_wifi_ctx.ssid);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Load LTE configuration from NVS
 */
static esp_err_t load_lte_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading LTE config from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Read LTE config blob (only the persistent fields)
  typedef struct {
    char apn[64];
    char username[32];
    char password[32];
    char modem_name[32];
    uint32_t max_reconnect_attempts;
    uint32_t reconnect_timeout_ms;
    bool auto_reconnect;
    lte_handler_comm_type_t comm_type;
    uint8_t pwr_pin;
    uint8_t rst_pin;
  } lte_config_persistent_t;

  lte_config_persistent_t lte_cfg;
  size_t required_size = sizeof(lte_config_persistent_t);
  err = nvs_get_blob(nvs_handle, NVS_KEY_LTE_CONFIG, &lte_cfg, &required_size);

  if (err == ESP_OK) {
    // Copy to global context
    strncpy(g_lte_ctx.apn, lte_cfg.apn, sizeof(g_lte_ctx.apn) - 1);
    strncpy(g_lte_ctx.username, lte_cfg.username, sizeof(g_lte_ctx.username) - 1);
    strncpy(g_lte_ctx.password, lte_cfg.password, sizeof(g_lte_ctx.password) - 1);
    strncpy(g_lte_ctx.modem_name, lte_cfg.modem_name, sizeof(g_lte_ctx.modem_name) - 1);
    g_lte_ctx.max_reconnect_attempts = lte_cfg.max_reconnect_attempts;
    g_lte_ctx.reconnect_timeout_ms = lte_cfg.reconnect_timeout_ms;
    g_lte_ctx.auto_reconnect = lte_cfg.auto_reconnect;
    g_lte_ctx.comm_type = lte_cfg.comm_type;
    g_lte_ctx.pwr_pin = lte_cfg.pwr_pin;
    g_lte_ctx.rst_pin = lte_cfg.rst_pin;

    ESP_LOGI(TAG, "LTE config loaded - APN: %s", g_lte_ctx.apn);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "LTE config not found in NVS, using defaults");
    err = ESP_OK; // Not an error, use defaults
  } else {
    ESP_LOGE(TAG, "Error reading LTE config: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save LTE configuration to NVS
 */
esp_err_t save_lte_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving LTE config to NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Prepare persistent data (exclude runtime fields like task_handle)
  typedef struct {
    char apn[64];
    char username[32];
    char password[32];
    char modem_name[32];
    uint32_t max_reconnect_attempts;
    uint32_t reconnect_timeout_ms;
    bool auto_reconnect;
    lte_handler_comm_type_t comm_type;
    uint8_t pwr_pin;
    uint8_t rst_pin;
  } lte_config_persistent_t;

  lte_config_persistent_t lte_cfg = {
      .max_reconnect_attempts = g_lte_ctx.max_reconnect_attempts,
      .reconnect_timeout_ms = g_lte_ctx.reconnect_timeout_ms,
      .auto_reconnect = g_lte_ctx.auto_reconnect,
      .comm_type = g_lte_ctx.comm_type,
      .pwr_pin = g_lte_ctx.pwr_pin,
      .rst_pin = g_lte_ctx.rst_pin};
  strncpy(lte_cfg.apn, g_lte_ctx.apn, sizeof(lte_cfg.apn) - 1);
  strncpy(lte_cfg.username, g_lte_ctx.username, sizeof(lte_cfg.username) - 1);
  strncpy(lte_cfg.password, g_lte_ctx.password, sizeof(lte_cfg.password) - 1);
  strncpy(lte_cfg.modem_name, g_lte_ctx.modem_name, sizeof(lte_cfg.modem_name) - 1);

  // Write LTE config blob
  err = nvs_set_blob(nvs_handle, NVS_KEY_LTE_CONFIG, &lte_cfg,
                     sizeof(lte_config_persistent_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing LTE config: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "LTE config saved - APN: %s", g_lte_ctx.apn);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Load MQTT configuration from NVS
 */
static esp_err_t load_mqtt_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading MQTT config from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Read MQTT config blob
  size_t required_size = sizeof(mqtt_config_context_t);
  err = nvs_get_blob(nvs_handle, NVS_KEY_MQTT_CONFIG, &g_mqtt_ctx,
                     &required_size);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "MQTT config loaded - Broker: %s", g_mqtt_ctx.broker_uri);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "MQTT config not found in NVS, using defaults");
    err = ESP_OK; // Not an error, use defaults
  } else {
    ESP_LOGE(TAG, "Error reading MQTT config: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save MQTT configuration to NVS
 */
esp_err_t save_mqtt_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving MQTT config to NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Write MQTT config blob
  err = nvs_set_blob(nvs_handle, NVS_KEY_MQTT_CONFIG, &g_mqtt_ctx,
                     sizeof(mqtt_config_context_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing MQTT config: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "MQTT config saved - Broker: %s", g_mqtt_ctx.broker_uri);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Load HTTP configuration from NVS
 */
static esp_err_t load_http_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading HTTP config from NVS...");
  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;

  size_t required_size = sizeof(http_config_data_t);
  err = nvs_get_blob(nvs_handle, NVS_KEY_HTTP_CONFIG, &g_http_cfg, &required_size);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HTTP config loaded - URL: %s", g_http_cfg.server_url);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "HTTP config not found in NVS, using defaults");
    err = ESP_OK;
  } else {
    ESP_LOGE(TAG, "Error reading HTTP config: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save HTTP configuration to NVS
 */
esp_err_t save_http_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving HTTP config to NVS...");
  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(nvs_handle, NVS_KEY_HTTP_CONFIG, &g_http_cfg,
                     sizeof(http_config_data_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing HTTP config: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "HTTP config saved - URL: %s", g_http_cfg.server_url);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Load CoAP configuration from NVS
 */
static esp_err_t load_coap_config_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Loading CoAP config from NVS...");
  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;

  size_t required_size = sizeof(coap_config_data_t);
  err = nvs_get_blob(nvs_handle, NVS_KEY_COAP_CONFIG, &g_coap_cfg, &required_size);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "CoAP config loaded - Host: %s", g_coap_cfg.host);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "CoAP config not found in NVS, using defaults");
    err = ESP_OK;
  } else {
    ESP_LOGE(TAG, "Error reading CoAP config: %s", esp_err_to_name(err));
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save CoAP configuration to NVS
 */
esp_err_t save_coap_config_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGI(TAG, "Saving CoAP config to NVS...");
  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(nvs_handle, NVS_KEY_COAP_CONFIG, &g_coap_cfg,
                     sizeof(coap_config_data_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error writing CoAP config: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "CoAP config saved - Host: %s", g_coap_cfg.host);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief Save WAN MCU firmware OTA URL to NVS.
 */
esp_err_t save_fota_wan_url_to_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;
  err = nvs_set_str(nvs_handle, NVS_KEY_FOTA_WAN_URL, fota_handler_get_url());
  if (err == ESP_OK) err = nvs_commit(nvs_handle);
  nvs_close(nvs_handle);
  if (err == ESP_OK)
    ESP_LOGI(TAG, "FOTA WAN URL saved: %s", fota_handler_get_url());
  return err;
}

/**
 * @brief Load WAN MCU firmware OTA URL from NVS (call at startup).
 */
static esp_err_t load_fota_wan_url_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) return err;
  char buf[FOTA_CONFIG_FIRMWARE_URL_MAX_LEN];
  size_t len = sizeof(buf);
  err = nvs_get_str(nvs_handle, NVS_KEY_FOTA_WAN_URL, buf, &len);
  nvs_close(nvs_handle);
  if (err == ESP_OK && len > 1) {
    fota_handler_set_url(buf);
    ESP_LOGI(TAG, "FOTA WAN URL loaded: %s", buf);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK; /* use default from fota_config.h */
  }
  return err;
}

/**
 * @brief Load all configurations from NVS (call at startup)
 */
static esp_err_t load_all_configs_from_nvs(void) {
  esp_err_t err;

  ESP_LOGI(TAG, "Loading all configurations from NVS...");

  // Load internet and server type
  err = load_internet_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load internet config");
  }

  err = load_server_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load server config");
  }

  // Load WiFi config
  err = load_wifi_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load WiFi config");
  }

  // Load LTE config
  err = load_lte_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load LTE config");
  }

  // Load MQTT config
  err = load_mqtt_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load MQTT config");
  }

  // Load HTTP config
  err = load_http_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load HTTP config");
  }

  // Load CoAP config
  err = load_coap_config_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load CoAP config");
  }

  // Load FOTA WAN URL
  err = load_fota_wan_url_from_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load FOTA WAN URL");
  }

  ESP_LOGI(TAG, "Configuration loading complete");
  return ESP_OK;
}

/**
 * @brief Erase all gateway configurations from NVS
 */
esp_err_t erase_all_configs_from_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  ESP_LOGW(TAG, "Erasing all configurations from NVS...");

  err = nvs_open_handle(&nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  // Erase all keys in the namespace
  err = nvs_erase_all(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error erasing NVS: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return err;
  }

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS erase: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "All configurations erased from NVS");
  }

  nvs_close(nvs_handle);
  return err;
}

static bool is_first_boot(void) {
    nvs_handle_t handle;
    uint8_t initialized = 0;
    
    if (nvs_open(NVS_NS_GATEWAY, NVS_READONLY, &handle) == ESP_OK) {
        esp_err_t err = nvs_get_u8(handle, NVS_KEY_INITIALIZED, &initialized);
        nvs_close(handle);
        return (err != ESP_OK || initialized == 0);
    }
    
    return true; // Namespace doesn't exist = first boot
}

static void mark_initialized(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NS_GATEWAY, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_INITIALIZED, 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/**
 * @brief Initialize configuration - auto-saves defaults on first boot
 */
esp_err_t config_init(void) {
    ESP_LOGI(TAG, "Initializing configuration system...");
    
    if (is_first_boot()) {
        ESP_LOGI(TAG, "First boot detected - saving default configuration");
        
        // Initialize MQTT defaults
        memset(&g_mqtt_ctx, 0, sizeof(g_mqtt_ctx));
        strncpy(g_mqtt_ctx.broker_uri,      MQTT_DEFAULT_BROKER,    sizeof(g_mqtt_ctx.broker_uri)      - 1);
        strncpy(g_mqtt_ctx.device_token,    MQTT_DEFAULT_TOKEN,     sizeof(g_mqtt_ctx.device_token)    - 1);
        strncpy(g_mqtt_ctx.publish_topic,   MQTT_DEFAULT_PUB_TOPIC, sizeof(g_mqtt_ctx.publish_topic)   - 1);
        strncpy(g_mqtt_ctx.subscribe_topic, MQTT_DEFAULT_SUB_TOPIC, sizeof(g_mqtt_ctx.subscribe_topic) - 1);
        strncpy(g_mqtt_ctx.attribute_topic, MQTT_DEFAULT_ATTR_TOPIC,sizeof(g_mqtt_ctx.attribute_topic) - 1);
        g_mqtt_ctx.keepalive_s = MQTT_DEFAULT_KEEPALIVE_S;
        g_mqtt_ctx.timeout_ms  = MQTT_DEFAULT_TIMEOUT_MS;

        // Initialize HTTP defaults
        memset(&g_http_cfg, 0, sizeof(g_http_cfg));
        strncpy(g_http_cfg.server_url, HTTP_DEFAULT_URL, sizeof(g_http_cfg.server_url) - 1);
        strncpy(g_http_cfg.auth_token, HTTP_DEFAULT_TOKEN, sizeof(g_http_cfg.auth_token) - 1);
        g_http_cfg.port = HTTP_DEFAULT_PORT;
        g_http_cfg.use_tls = false;
        g_http_cfg.verify_server = false;
        g_http_cfg.timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;

        // Initialize CoAP defaults
        memset(&g_coap_cfg, 0, sizeof(g_coap_cfg));
        strncpy(g_coap_cfg.host, COAP_DEFAULT_HOST, sizeof(g_coap_cfg.host) - 1);
        strncpy(g_coap_cfg.resource_path, COAP_DEFAULT_RESOURCE, sizeof(g_coap_cfg.resource_path) - 1);
        strncpy(g_coap_cfg.device_token, COAP_DEFAULT_TOKEN, sizeof(g_coap_cfg.device_token) - 1);
        g_coap_cfg.port = COAP_DEFAULT_PORT;
        g_coap_cfg.use_dtls = false;
        g_coap_cfg.ack_timeout_ms = COAP_DEFAULT_ACK_TO_MS;
        g_coap_cfg.max_retransmit = COAP_DEFAULT_MAX_RTX;
        g_coap_cfg.rpc_poll_interval_ms = COAP_DEFAULT_RPC_POLL_MS;

        // Save to NVS
        save_wifi_config_to_nvs();
        save_lte_config_to_nvs();
        save_mqtt_config_to_nvs();
        save_http_config_to_nvs();
        save_coap_config_to_nvs();
        save_internet_config_to_nvs();
        save_server_config_to_nvs();
        save_fota_wan_url_to_nvs();
        
        mark_initialized();
        
        ESP_LOGI(TAG, "Default configuration saved");
    } else {
        ESP_LOGI(TAG, "Loading existing configuration");
        load_all_configs_from_nvs();
    }
    
    return ESP_OK;
}

/**
 * @brief Compare current WAN hardware stack ID with the NVS-stored value.
 *
 * If the stack ID has changed (i.e. the hardware connector changed),
 * all LTE configuration is erased from NVS so it is re-entered for the
 * new modem.  The current ID is then persisted.
 *
 * For WAN the hardware module always reports "001" (single stack).
 */
esp_err_t config_init_wan_stack_id(void) {
    const char *cur_id = stack_handler_get_module_id(1);  /* adapter IOX 0x21 — P00-P03 = DEV_ID */

    nvs_handle_t h;
    char old_id[8] = "000";

    /* Load previously stored stack ID */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(old_id);
        nvs_get_str(h, NVS_KEY_WAN_STACK_ID, old_id, &len);
        nvs_close(h);
    }

    if (strcmp(cur_id, old_id) != 0) {
        ESP_LOGW(TAG, "WAN stack module changed: '%s' -> '%s', clearing LTE config",
                 old_id, cur_id);
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_LTE_CONFIG);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    /* Persist current ID */
    strncpy(g_stack_id_wan, cur_id, sizeof(g_stack_id_wan) - 1);
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_WAN_STACK_ID, g_stack_id_wan);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "WAN stack ID: %s", g_stack_id_wan);
    return ESP_OK;
}

const char *config_get_wan_stack_id(void) { return g_stack_id_wan; }