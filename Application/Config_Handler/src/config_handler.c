/*
 * Config handler module for ESP32-S3 board.
 * This module processes configuration commands received from the gateway
 * All comments are in English for clarity.
 */

#include "config_handler.h"
#include "rbg_handler.h"
#include "http_handler.h"
#include "coap_handler.h"
#include "esp_log.h"
#include <ctype.h>

static const char *TAG = "config_handler";

// Configuration command constants
#define CONFIG_CMD_PREFIX_LEN 2
#define CONFIG_CMD_MIN_LEN 5

/* Field parsing helpers for string tokenization (colon-separated fields) */
#define _PARSE_FIELD_START(i, ptr, sep, sep_count) ((i) == 0 ? (ptr) : sep[(i)-1] + 1)
#define _PARSE_FIELD_END(i, end, sep, sep_count)   ((i) < (sep_count) ? sep[i] : (end))
#define _PARSE_FIELD_LEN(i, ptr, end, sep, sep_count) \
    ((int)(_PARSE_FIELD_END(i, end, sep, sep_count) - _PARSE_FIELD_START(i, ptr, sep, sep_count)))

// External global config contexts (defined in wifi_connect.c, lte_connect.c, mqtt_handler.c)
extern wifi_config_context_t g_wifi_ctx;
extern lte_config_context_t g_lte_ctx;
extern mqtt_config_context_t g_mqtt_ctx;

// HTTP and CoAP config globals (used by config_handler + http_handler/coap_handler)
http_config_data_t g_http_cfg;
coap_config_data_t g_coap_cfg;

// Thread-safety: Mutex for config context protection
static SemaphoreHandle_t g_config_context_mutex = NULL;

// Queue handles
QueueHandle_t g_wifi_config_queue = NULL;
QueueHandle_t g_lte_config_queue = NULL;
QueueHandle_t g_mqtt_config_queue = NULL;
QueueHandle_t g_config_handler_queue = NULL;

// Global config contexts
config_internet_type_t g_internet_type = CONFIG_INTERNET_WIFI;  
config_server_type_t g_server_type = CONFIG_SERVERTYPE_MQTT;  /* default to MQTT */
bool is_internet_connected = false;

static bool config_handler_running = false;
static TaskHandle_t config_handler_task_handle = NULL;

//pre define functions
static esp_err_t config_parse_wifi(const char *data, uint16_t len, wifi_config_data_t *cfg);
static esp_err_t config_parse_lte(const char *data, uint16_t len, lte_config_data_t *cfg);
static esp_err_t config_parse_mqtt(const char *data, uint16_t len, mqtt_config_data_t *cfg);
static esp_err_t config_parse_internet(const char *data, uint16_t len, config_internet_type_t *type);
static esp_err_t config_parse_server_type(const char *data, uint16_t len, config_server_type_t *type);
static esp_err_t config_parse_http(const char *data, uint16_t len, http_config_data_t *cfg);
static esp_err_t config_parse_coap(const char *data, uint16_t len, coap_config_data_t *cfg);

/**
 * @brief Parse command type from 2-character prefix
 * @param cmd Command string
 * @param len Command length
 * @return config_type_t Command type enum
 */
config_type_t config_parse_type(const char *cmd, uint16_t len) {
    if (len < CONFIG_CMD_PREFIX_LEN) {
        ESP_LOGW(TAG, "Command too short: %d bytes", len);
        return CONFIG_TYPE_UNKNOWN;
    }
    
    // Check first 2 characters
    if (cmd[0] == 'W' && cmd[1] == 'F') {
        return CONFIG_TYPE_WIFI;
    } else if (cmd[0] == 'M' && cmd[1] == 'Q') {
        return CONFIG_TYPE_MQTT;
    } else if (cmd[0] == 'F' && cmd[1] == 'W') {
        return CONFIG_UPDATE_FIRMWARE;
    } else if (cmd[0] == 'L' && cmd[1] == 'T') {
        return CONFIG_TYPE_LTE;
    } else if (cmd[0] == 'I' && cmd[1] == 'N') {
        return CONFIG_TYPE_INTERNET;
    } else if (cmd[0] == 'M' && cmd[1] == 'L') {
        return CONFIG_TYPE_MCU_LAN;
    } else if (cmd[0] == 'S' && cmd[1] == 'V') {
        return CONFIG_TYPE_SERVER;
    } else if (cmd[0] == 'H' && cmd[1] == 'P') {
        return CONFIG_TYPE_HTTP;
    } else if (cmd[0] == 'C' && cmd[1] == 'P') {
        return CONFIG_TYPE_COAP;
    }
    
    ESP_LOGW(TAG, "Unknown command prefix: %c%c", cmd[0], cmd[1]);
    return CONFIG_TYPE_UNKNOWN;
}

// ===== Thread-Safe Config Access Functions =====

/**
 * @brief Thread-safe read of WiFi config
 * @param out_cfg Output buffer for config
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if mutex timeout
 */
esp_err_t config_get_wifi_safe(wifi_config_context_t *out_cfg) {
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(out_cfg, &g_wifi_ctx, sizeof(wifi_config_context_t));
        xSemaphoreGive(g_config_context_mutex);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Thread-safe update of WiFi config
 * @param new_cfg New configuration data
 * @return ESP_OK on success
 */
esp_err_t config_update_wifi_safe(const wifi_config_data_t *new_cfg) {
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // wifi_config_context_t is flat - copy fields directly
        memcpy(&g_wifi_ctx, new_cfg, sizeof(wifi_config_data_t));
        xSemaphoreGive(g_config_context_mutex);
        
        ESP_LOGI(TAG, "WiFi config updated safely");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Thread-safe read of LTE config
 */
esp_err_t config_get_lte_safe(lte_config_context_t *out_cfg) {
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(out_cfg, &g_lte_ctx, sizeof(lte_config_context_t));
        xSemaphoreGive(g_config_context_mutex);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Thread-safe update of LTE config
 */
esp_err_t config_update_lte_safe(const lte_config_data_t *new_cfg) {
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        /* Explicitly copy config fields; do NOT touch runtime state fields
         * (initialized, task_running, task_handle). */
        strncpy(g_lte_ctx.modem_name, new_cfg->modem_name, sizeof(g_lte_ctx.modem_name) - 1);
        strncpy(g_lte_ctx.apn,        new_cfg->apn,        sizeof(g_lte_ctx.apn)        - 1);
        strncpy(g_lte_ctx.username,   new_cfg->username,   sizeof(g_lte_ctx.username)   - 1);
        strncpy(g_lte_ctx.password,   new_cfg->password,   sizeof(g_lte_ctx.password)   - 1);
        g_lte_ctx.comm_type               = new_cfg->comm_type;
        g_lte_ctx.auto_reconnect          = new_cfg->auto_reconnect;
        g_lte_ctx.reconnect_timeout_ms    = new_cfg->reconnect_timeout_ms;
        g_lte_ctx.max_reconnect_attempts  = new_cfg->max_reconnect_attempts;
        g_lte_ctx.pwr_pin                 = new_cfg->pwr_pin;
        g_lte_ctx.rst_pin                 = new_cfg->rst_pin;
        xSemaphoreGive(g_config_context_mutex);
        
        ESP_LOGI(TAG, "LTE config updated safely (modem=%s, apn=%s)",
                 g_lte_ctx.modem_name, g_lte_ctx.apn);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Thread-safe read of MQTT config
 */
esp_err_t config_get_mqtt_safe(mqtt_config_context_t *out_cfg) {
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(out_cfg, &g_mqtt_ctx, sizeof(mqtt_config_context_t));
        xSemaphoreGive(g_config_context_mutex);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Thread-safe update of MQTT config
 */
esp_err_t config_update_mqtt_safe(const mqtt_config_data_t *new_cfg) {
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_config_context_mutex == NULL) {
        ESP_LOGE(TAG, "Config mutex not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_config_context_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // mqtt_config_context_t is a flat structure -just copy directly
        memcpy(&g_mqtt_ctx, new_cfg, sizeof(mqtt_config_data_t));
        xSemaphoreGive(g_config_context_mutex);
        
        ESP_LOGI(TAG, "MQTT config updated safely");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire config mutex (timeout)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Parse WiFi configuration from command string
 * Format: "WF:SSID:PASSWORD:AUTH_MODE"
 * Example: "WF:MyWiFi:MyPassword123:PERSONAL"
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output WiFi config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t config_parse_wifi(const char *data, uint16_t len, wifi_config_data_t *cfg) {
    if (!data || !cfg || len < CONFIG_CMD_MIN_LEN) {
        ESP_LOGE(TAG, "WiFi config parse: invalid parameters (data=%p, cfg=%p, len=%u)", 
                 data, cfg, len);
        return ESP_FAIL;
    }
    
    memset(cfg, 0, sizeof(wifi_config_data_t));
    ESP_LOGD(TAG, "Parsing WiFi config: %.*s", len, data);
    
    // Parse format: "WF:SSID:PASSWORD:AUTH_MODE" or "WF:SSID:PASSWORD:USERNAME:AUTH_MODE"
    const char *ptr = data + 3; // Skip "WF:"
    const char *end = data + len;
    
    // Find first colon (after SSID)
    const char *first_colon = strchr(ptr, ':');
    if (!first_colon || first_colon >= end) {
        ESP_LOGE(TAG, "WiFi config format error: missing SSID separator");
        return ESP_FAIL;
    }
    
    // Extract SSID
    int ssid_len = first_colon - ptr;
    if (ssid_len <= 0 || ssid_len >= sizeof(cfg->ssid)) {
        ESP_LOGE(TAG, "WiFi SSID length invalid: %d", ssid_len);
        return ESP_FAIL;
    }
    memcpy(cfg->ssid, ptr, ssid_len);
    cfg->ssid[ssid_len] = '\0';
    
    // Find second colon (after password)
    const char *second_colon = strchr(first_colon + 1, ':');
    if (!second_colon || second_colon >= end) {
        ESP_LOGE(TAG, "WiFi config format error: missing password separator");
        return ESP_FAIL;
    }
    
    // Extract password
    int pass_len = second_colon - first_colon - 1;
    if (pass_len < 0 || pass_len >= sizeof(cfg->password)) {
        ESP_LOGE(TAG, "WiFi password length invalid: %d", pass_len);
        return ESP_FAIL;
    }
    if (pass_len > 0) {
        memcpy(cfg->password, first_colon + 1, pass_len);
        cfg->password[pass_len] = '\0';
    }
    
    // Find third colon (might be username or auth_mode)
    const char *third_colon = strchr(second_colon + 1, ':');
    
    if (third_colon && third_colon < end) {
        // Format: "WF:SSID:PASSWORD:USERNAME:AUTH_MODE"
        // Extract username
        int username_len = third_colon - second_colon - 1;
        if (username_len > 0 && username_len < sizeof(cfg->username)) {
            memcpy(cfg->username, second_colon + 1, username_len);
            cfg->username[username_len] = '\0';
        }
        
        // Extract auth_mode
        const char *auth_start = third_colon + 1;
        int auth_len = end - auth_start;
        if (auth_len > 0) {
            if (strncmp(auth_start, "ENTERPRISE", 10) == 0) {
                cfg->auth_mode = WIFI_AUTH_MODE_ENTERPRISE;
            } else {
                cfg->auth_mode = WIFI_AUTH_MODE_PERSONAL;
            }
        } else {
            cfg->auth_mode = WIFI_AUTH_MODE_PERSONAL;
        }
    } else {
        // Format: "WF:SSID:PASSWORD:AUTH_MODE"
        const char *auth_start = second_colon + 1;
        int auth_len = end - auth_start;
        if (auth_len > 0) {
            if (strncmp(auth_start, "ENTERPRISE", 10) == 0) {
                cfg->auth_mode = WIFI_AUTH_MODE_ENTERPRISE;
            } else {
                cfg->auth_mode = WIFI_AUTH_MODE_PERSONAL;
            }
        } else {
            cfg->auth_mode = WIFI_AUTH_MODE_PERSONAL;
        }
    }
    
    ESP_LOGI(TAG, "Parsed WiFi config - SSID: '%s', Pass: '%s', Username: '%s', Auth: %d", 
             cfg->ssid, cfg->password, cfg->username, cfg->auth_mode);
    return ESP_OK;
}

/**
 * @brief Parse LTE configuration from command string
 * Format: "LT:MODEM_NAME:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:RECONNECT_TIMEOUT:MAX_RECONNECT:PWR_PIN:RST_PIN"
 * Example: "LT:A7600C1:v-internet:user:pass:USB:true:30000:0:WK:PE"
 * Note: USERNAME and PASSWORD can be empty (consecutive colons allowed).
 *       PWR_PIN / RST_PIN are TCA pin labels: "WK"="WAKE#"(11), "PE"="PERST#"(12),
 *       or "01".."11" for numbered GPIO pins (0-10).  Omit or use "" to keep default.
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output LTE config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
static uint8_t parse_tca_pin_label(const char *s, int len) {
    if (len == 2) {
        if ((s[0] == 'W' || s[0] == 'w') && (s[1] == 'K' || s[1] == 'k')) return 11; /* STACK_GPIO_PIN_WAKE  */
        if ((s[0] == 'P' || s[0] == 'p') && (s[1] == 'E' || s[1] == 'e')) return 12; /* STACK_GPIO_PIN_PERST */
        /* "01" - "11" -> index 0-10 */
        if (s[0] == '0' || s[0] == '1') {
            int num = (s[0] - '0') * 10 + (s[1] - '0');
            if (num >= 1 && num <= 11) return (uint8_t)(num - 1);
        }
    }
    return 0xFF; /* STACK_GPIO_PIN_NONE */
}

static esp_err_t config_parse_lte(const char *data, uint16_t len, lte_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }

    memset(cfg, 0, sizeof(lte_config_data_t));
    /* Default TCA pins if not supplied in command */
    cfg->pwr_pin = 11; /* STACK_GPIO_PIN_WAKE  */
    cfg->rst_pin = 12; /* STACK_GPIO_PIN_PERST */

    /* Parse format:
     * "LT:MODEM_NAME:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:RECONNECT_TIMEOUT:MAX_RECONNECT:PWR_PIN:RST_PIN"
     * Field indices (0-based after the "LT:" prefix):
     *  0 MODEM_NAME   1 APN   2 USERNAME   3 PASSWORD
     *  4 COMM_TYPE    5 AUTO_RECONNECT   6 RECONNECT_TIMEOUT   7 MAX_RECONNECT
     *  8 PWR_PIN      9 RST_PIN
     */
    const char *ptr = data + 3; /* skip "LT:" */
    const char *end = data + len;
    const char *sep[11] = {0};
    int sep_count = 0;

    const char *p = ptr;
    while (p < end && sep_count < 11) {
        const char *colon = memchr(p, ':', end - p);
        if (!colon) break;
        sep[sep_count++] = colon;
        p = colon + 1;
    }

    /* Must have at least MODEM_NAME, APN, and COMM_TYPE (fields 0,1,4) */
    if (sep_count < 4) {
        ESP_LOGE(TAG, "LTE config: insufficient fields (need at least 5)");
        return ESP_FAIL;
    }

    /* Field 0: MODEM_NAME */
    {
        int fl = _PARSE_FIELD_LEN(0, ptr, end, sep, sep_count);
        if (fl > 0 && fl < (int)sizeof(cfg->modem_name)) {
            memcpy(cfg->modem_name, _PARSE_FIELD_START(0, ptr, sep, sep_count), fl);
            cfg->modem_name[fl] = '\0';
        }
    }

    /* Field 1: APN (required) */
    {
        int fl = _PARSE_FIELD_LEN(1, ptr, end, sep, sep_count);
        if (fl <= 0) {
            ESP_LOGE(TAG, "LTE config: APN is empty");
            return ESP_FAIL;
        }
        if (fl >= (int)sizeof(cfg->apn)) {
            ESP_LOGE(TAG, "LTE config: APN too long");
            return ESP_FAIL;
        }
        memcpy(cfg->apn, _PARSE_FIELD_START(1, ptr, sep, sep_count), fl);
        cfg->apn[fl] = '\0';
    }

    /* Field 2: USERNAME (optional) */
    if (sep_count >= 2) {
        int fl = _PARSE_FIELD_LEN(2, ptr, end, sep, sep_count);
        if (fl > 0 && fl < (int)sizeof(cfg->username)) {
            memcpy(cfg->username, _PARSE_FIELD_START(2, ptr, sep, sep_count), fl);
            cfg->username[fl] = '\0';
        }
    }

    /* Field 3: PASSWORD (optional) */
    if (sep_count >= 3) {
        int fl = _PARSE_FIELD_LEN(3, ptr, end, sep, sep_count);
        if (fl > 0 && fl < (int)sizeof(cfg->password)) {
            memcpy(cfg->password, _PARSE_FIELD_START(3, ptr, sep, sep_count), fl);
            cfg->password[fl] = '\0';
        }
    }

    /* Field 4: COMM_TYPE */
    if (sep_count >= 4) {
        const char *s = _PARSE_FIELD_START(4, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(4, ptr, end, sep, sep_count);
        if (fl >= 3 && strncmp(s, "USB", 3) == 0) {
            cfg->comm_type = LTE_HANDLER_USB;
        } else if (fl >= 4 && strncmp(s, "UART", 4) == 0) {
            cfg->comm_type = LTE_HANDLER_UART;
        } else {
            ESP_LOGW(TAG, "LTE config: unknown COMM_TYPE, defaulting to USB");
            cfg->comm_type = LTE_HANDLER_USB;
        }
    }

    /* Field 5: AUTO_RECONNECT (optional, true/false) */
    if (sep_count >= 5) {
        const char *s = _PARSE_FIELD_START(5, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(5, ptr, end, sep, sep_count);
        cfg->auto_reconnect = (fl >= 4 && strncmp(s, "true", 4) == 0);
    }

    /* Field 6: RECONNECT_TIMEOUT_MS (optional, integer) */
    if (sep_count >= 6) {
        const char *s = _PARSE_FIELD_START(6, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(6, ptr, end, sep, sep_count);
        if (fl > 0 && fl < 12) {
            char buf[12] = {0};
            memcpy(buf, s, fl);
            cfg->reconnect_timeout_ms = (uint32_t)atoi(buf);
        }
    }

    /* Field 7: MAX_RECONNECT_ATTEMPTS (optional, integer) */
    if (sep_count >= 7) {
        const char *s = _PARSE_FIELD_START(7, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(7, ptr, end, sep, sep_count);
        if (fl > 0 && fl < 12) {
            char buf[12] = {0};
            memcpy(buf, s, fl);
            cfg->max_reconnect_attempts = (uint32_t)atoi(buf);
        }
    }

    /* Field 8: PWR_PIN (optional, e.g. "WK", "PE", "01".."11") */
    if (sep_count >= 8) {
        const char *s = _PARSE_FIELD_START(8, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(8, ptr, end, sep, sep_count);
        uint8_t p = parse_tca_pin_label(s, fl);
        if (p != 0xFF) cfg->pwr_pin = p;
    }

    /* Field 9: RST_PIN (optional) */
    if (sep_count >= 9) {
        const char *s = _PARSE_FIELD_START(9, ptr, sep, sep_count);
        int fl = _PARSE_FIELD_LEN(9, ptr, end, sep, sep_count);
        uint8_t r = parse_tca_pin_label(s, fl);
        if (r != 0xFF) cfg->rst_pin = r;
    }

    ESP_LOGI(TAG, "Parsed LTE config - Modem: '%s', APN: '%s', Usr: '%s', "
                  "CommType: %d, AutoConn: %d, Timeout: %lu, MaxRetry: %lu, "
                  "PwrPin: %u, RstPin: %u",
             cfg->modem_name, cfg->apn, cfg->username,
             cfg->comm_type, cfg->auto_reconnect,
             (unsigned long)cfg->reconnect_timeout_ms,
             (unsigned long)cfg->max_reconnect_attempts,
             cfg->pwr_pin, cfg->rst_pin);
    return ESP_OK;
}

/**
 * @brief Parse MQTT configuration from command string
 * Format: "MQ:BROKER_URI|DEVICE_TOKEN|SUBSCRIBE_TOPIC|PUBLISH_TOPIC|ATTRIBUTE_TOPIC"
 * Example: "MQ:mqtt://demo.thingsboard.io:1883|myDeviceToken123|subTopic|pubTopic|attrTopic"
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output MQTT config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t config_parse_mqtt(const char *data, uint16_t len, mqtt_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }
    
    memset(cfg, 0, sizeof(mqtt_config_data_t));
    
    // Parse format: "MQ:BROKER_URI|DEVICE_TOKEN|SUBSCRIBE_TOPIC|PUBLISH_TOPIC|ATTRIBUTE_TOPIC"
    const char *ptr = data + 3; // Skip "MQ:"
    const char *end = data + len;
    
    // Parse broker URI
    const char *first_pipe = strchr(ptr, '|');
    if (!first_pipe || first_pipe >= end) {
        ESP_LOGE(TAG, "MQTT config: missing broker URI separator");
        return ESP_FAIL;
    }
    
    int uri_len = first_pipe - ptr;
    if (uri_len <= 0 || uri_len >= sizeof(cfg->broker_uri)) {
        ESP_LOGE(TAG, "MQTT broker URI length invalid: %d", uri_len);
        return ESP_FAIL;
    }
    
    memcpy(cfg->broker_uri, ptr, uri_len);
    cfg->broker_uri[uri_len] = '\0';
    
    // Parse device token
    ptr = first_pipe + 1;
    const char *second_pipe = strchr(ptr, '|');
    if (!second_pipe || second_pipe >= end) {
        ESP_LOGE(TAG, "MQTT config: missing device token separator");
        return ESP_FAIL;
    }
    
    int token_len = second_pipe - ptr;
    if (token_len <= 0 || token_len >= sizeof(cfg->device_token)) {
        ESP_LOGE(TAG, "MQTT token length invalid: %d", token_len);
        return ESP_FAIL;
    }
    
    memcpy(cfg->device_token, ptr, token_len);
    cfg->device_token[token_len] = '\0';
    
    // Parse subscribe topic
    ptr = second_pipe + 1;
    const char *third_pipe = strchr(ptr, '|');
    if (!third_pipe || third_pipe >= end) {
        ESP_LOGE(TAG, "MQTT config: missing subscribe topic separator");
        return ESP_FAIL;
    }
    
    int sub_topic_len = third_pipe - ptr;
    if (sub_topic_len <= 0 || sub_topic_len >= sizeof(cfg->subscribe_topic)) {
        ESP_LOGE(TAG, "MQTT subscribe topic length invalid: %d", sub_topic_len);
        return ESP_FAIL;
    }
    
    memcpy(cfg->subscribe_topic, ptr, sub_topic_len);
    cfg->subscribe_topic[sub_topic_len] = '\0';
    
    // Parse publish topic
    ptr = third_pipe + 1;
    const char *fourth_pipe = strchr(ptr, '|');
    if (!fourth_pipe || fourth_pipe >= end) {
        ESP_LOGE(TAG, "MQTT config: missing publish topic separator");
        return ESP_FAIL;
    }
    
    int pub_topic_len = fourth_pipe - ptr;
    if (pub_topic_len <= 0 || pub_topic_len >= sizeof(cfg->publish_topic)) {
        ESP_LOGE(TAG, "MQTT publish topic length invalid: %d", pub_topic_len);
        return ESP_FAIL;
    }
    
    memcpy(cfg->publish_topic, ptr, pub_topic_len);
    cfg->publish_topic[pub_topic_len] = '\0';
    
    // Parse attribute topic
    ptr = fourth_pipe + 1;
    const char *fifth_pipe = strchr(ptr, '|');
    int attr_topic_len;
    if (!fifth_pipe || fifth_pipe >= end) {
        // No more fields — attribute topic runs to end
        attr_topic_len = end - ptr;
    } else {
        attr_topic_len = fifth_pipe - ptr;
    }
    if (attr_topic_len <= 0 || attr_topic_len >= (int)sizeof(cfg->attribute_topic)) {
        ESP_LOGE(TAG, "MQTT attribute topic length invalid: %d", attr_topic_len);
        return ESP_FAIL;
    }
    memcpy(cfg->attribute_topic, ptr, attr_topic_len);
    cfg->attribute_topic[attr_topic_len] = '\0';

    // Optional field 5: keepalive_s
    if (fifth_pipe && fifth_pipe < end) {
        ptr = fifth_pipe + 1;
        const char *sixth_pipe = strchr(ptr, '|');
        int ka_len = sixth_pipe && sixth_pipe < end ? (int)(sixth_pipe - ptr) : (int)(end - ptr);
        if (ka_len > 0 && ka_len < 8) {
            char buf[8] = {0};
            memcpy(buf, ptr, ka_len);
            int ka = atoi(buf);
            if (ka > 0) cfg->keepalive_s = (uint16_t)ka;
        }
        // Optional field 6: timeout_ms
        if (sixth_pipe && sixth_pipe < end) {
            ptr = sixth_pipe + 1;
            int tmo_len = end - ptr;
            if (tmo_len > 0 && tmo_len < 12) {
                char buf[12] = {0};
                memcpy(buf, ptr, tmo_len);
                uint32_t tmo = (uint32_t)atoi(buf);
                if (tmo > 0) cfg->timeout_ms = tmo;
            }
        }
    }

    ESP_LOGI(TAG, "Parsed MQTT config - URI: '%s', Token: '%s', Sub: '%s', Pub: '%s', Attr: '%s', keepalive=%us, timeout=%lums",
             cfg->broker_uri, cfg->device_token, cfg->subscribe_topic, cfg->publish_topic, cfg->attribute_topic,
             cfg->keepalive_s, (unsigned long)cfg->timeout_ms);
    return ESP_OK;
}

/**
 * @brief Parse Internet configuration from command string
 * Format: "IN:TYPE"
 * Example: "IN:WIFI"
 * @param data Raw command data
 * @param len Command length
 * @param type Output Internet type enum
 * @return ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t config_parse_internet(const char *data, uint16_t len, config_internet_type_t *type) {
    if (!data || !type || len < 5) {
        return ESP_FAIL;
    }
    
    // Simple parsing: "IN:TYPE"
    const char *ptr = data + 3; // Skip "IN:"
    if (strncmp(ptr, "WIFI", 4) == 0) {
        *type = CONFIG_INTERNET_WIFI;
    } else if (strncmp(ptr, "LTE", 3) == 0) {
        *type = CONFIG_INTERNET_LTE;
    } else if (strncmp(ptr, "ETHERNET", 8) == 0) {
        *type = CONFIG_INTERNET_ETHERNET;
    } else if (strncmp(ptr, "NBIOT", 5) == 0) {
        *type = CONFIG_INTERNET_NBIOT;
    } else {
        *type = CONFIG_INTERNET_TYPE_UNKNOWN;
        ESP_LOGE(TAG, "Internet config type unknown");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Parsed Internet config type: %d", *type);
    return ESP_OK;
}

/**
 * @brief Parse server type from command string
 * Format: "SV:TYPE" where TYPE = 0 (MQTT), 1 (CoAP), 2 (HTTP)
 */
static esp_err_t config_parse_server_type(const char *data, uint16_t len, config_server_type_t *type) {
    if (!data || !type || len < 4) {
        return ESP_FAIL;
    }

    const char *ptr = data + 3; // Skip "SV:"
    int val = atoi(ptr);
    if (val >= 0 && val < CONFIG_SERVERTYPE_COUNT) {
        *type = (config_server_type_t)val;
        ESP_LOGI(TAG, "Parsed Server type: %d", *type);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Invalid server type value: %d", val);
    return ESP_FAIL;
}

/**
 * @brief Parse HTTP configuration from command string
 * Format: "HP:URL|AUTH_TOKEN|PORT|USE_TLS|VERIFY|TIMEOUT_MS"
 * Example: "HP:http://server:8080/path|myToken|8080|0|0|10000"
 */
static esp_err_t config_parse_http(const char *data, uint16_t len, http_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }

    memset(cfg, 0, sizeof(http_config_data_t));
    // Default values
    cfg->port = 80;
    cfg->timeout_ms = 10000;

    const char *ptr = data + 3; // Skip "HP:"
    const char *end = data + len;

    // Field 0: URL
    const char *sep1 = strchr(ptr, '|');
    if (!sep1 || sep1 >= end) { sep1 = end; }
    int url_len = sep1 - ptr;
    if (url_len > 0 && url_len < (int)sizeof(cfg->server_url)) {
        memcpy(cfg->server_url, ptr, url_len);
        cfg->server_url[url_len] = '\0';
    }
    if (sep1 >= end) goto done;

    // Field 1: auth_token
    ptr = sep1 + 1;
    const char *sep2 = strchr(ptr, '|');
    if (!sep2 || sep2 >= end) { sep2 = end; }
    int tok_len = sep2 - ptr;
    if (tok_len > 0 && tok_len < (int)sizeof(cfg->auth_token)) {
        memcpy(cfg->auth_token, ptr, tok_len);
        cfg->auth_token[tok_len] = '\0';
    }
    if (sep2 >= end) goto done;

    // Field 2: port
    ptr = sep2 + 1;
    const char *sep3 = strchr(ptr, '|');
    if (!sep3 || sep3 >= end) { sep3 = end; }
    cfg->port = (uint16_t)atoi(ptr);
    if (sep3 >= end) goto done;

    // Field 3: use_tls
    ptr = sep3 + 1;
    const char *sep4 = strchr(ptr, '|');
    if (!sep4 || sep4 >= end) { sep4 = end; }
    cfg->use_tls = (atoi(ptr) != 0);
    if (sep4 >= end) goto done;

    // Field 4: verify_server
    ptr = sep4 + 1;
    const char *sep5 = strchr(ptr, '|');
    if (!sep5 || sep5 >= end) { sep5 = end; }
    cfg->verify_server = (atoi(ptr) != 0);
    if (sep5 >= end) goto done;

    // Field 5: timeout_ms
    ptr = sep5 + 1;
    cfg->timeout_ms = (uint32_t)atoi(ptr);

done:
    ESP_LOGI(TAG, "Parsed HTTP config - URL: '%s', TLS: %d", cfg->server_url, cfg->use_tls);
    return ESP_OK;
}

/**
 * @brief Parse CoAP configuration from command string
 * Format: "CP:HOST|RESOURCE_PATH|TOKEN|PORT|USE_DTLS|ACK_TIMEOUT|MAX_RTX"
 * Example: "CP:server.io|/api/v1/{token}/telemetry|myToken|5683|0|2000|4"
 */
static esp_err_t config_parse_coap(const char *data, uint16_t len, coap_config_data_t *cfg) {
    if (!data || !cfg || len < 5) {
        return ESP_FAIL;
    }

    memset(cfg, 0, sizeof(coap_config_data_t));
    // Default values
    cfg->port = 5683;
    cfg->ack_timeout_ms = 2000;
    cfg->max_retransmit = 4;

    const char *ptr = data + 3; // Skip "CP:"
    const char *end = data + len;

    // Field 0: host
    const char *sep1 = strchr(ptr, '|');
    if (!sep1 || sep1 >= end) { sep1 = end; }
    int host_len = sep1 - ptr;
    if (host_len > 0 && host_len < (int)sizeof(cfg->host)) {
        memcpy(cfg->host, ptr, host_len);
        cfg->host[host_len] = '\0';
    }
    if (sep1 >= end) goto done;

    // Field 1: resource_path
    ptr = sep1 + 1;
    const char *sep2 = strchr(ptr, '|');
    if (!sep2 || sep2 >= end) { sep2 = end; }
    int path_len = sep2 - ptr;
    if (path_len > 0 && path_len < (int)sizeof(cfg->resource_path)) {
        memcpy(cfg->resource_path, ptr, path_len);
        cfg->resource_path[path_len] = '\0';
    }
    if (sep2 >= end) goto done;

    // Field 2: device_token
    ptr = sep2 + 1;
    const char *sep3 = strchr(ptr, '|');
    if (!sep3 || sep3 >= end) { sep3 = end; }
    int tok_len = sep3 - ptr;
    if (tok_len > 0 && tok_len < (int)sizeof(cfg->device_token)) {
        memcpy(cfg->device_token, ptr, tok_len);
        cfg->device_token[tok_len] = '\0';
    }
    if (sep3 >= end) goto done;

    // Field 3: port
    ptr = sep3 + 1;
    const char *sep4 = strchr(ptr, '|');
    if (!sep4 || sep4 >= end) { sep4 = end; }
    cfg->port = (uint16_t)atoi(ptr);
    if (sep4 >= end) goto done;

    // Field 4: use_dtls
    ptr = sep4 + 1;
    const char *sep5 = strchr(ptr, '|');
    if (!sep5 || sep5 >= end) { sep5 = end; }
    cfg->use_dtls = (atoi(ptr) != 0);
    if (sep5 >= end) goto done;

    // Field 5: ack_timeout_ms
    ptr = sep5 + 1;
    const char *sep6 = strchr(ptr, '|');
    if (!sep6 || sep6 >= end) { sep6 = end; }
    cfg->ack_timeout_ms = (uint32_t)atoi(ptr);
    if (sep6 >= end) goto done;

    // Field 6: max_retransmit
    ptr = sep6 + 1;
    const char *sep7 = strchr(ptr, '|');
    if (!sep7 || sep7 >= end) { sep7 = end; }
    cfg->max_retransmit = (uint8_t)atoi(ptr);
    if (sep7 >= end) goto done;

    // Field 7: rpc_poll_interval_ms (optional, 0 = use firmware default)
    ptr = sep7 + 1;
    {
        uint32_t poll_ms = (uint32_t)atoi(ptr);
        if (poll_ms > 0) cfg->rpc_poll_interval_ms = poll_ms;
    }

done:
    ESP_LOGI(TAG, "Parsed CoAP config - Host: '%s', Resource: '%s', poll_ms=%lu",
             cfg->host, cfg->resource_path, (unsigned long)cfg->rpc_poll_interval_ms);
    return ESP_OK;
}

/**
 * @brief Config handler task - receives raw commands and routes to specific queues
 * @param arg Task argument (unused)
 */
static void config_handler_task(void *arg) {
    config_command_t *cmd = NULL;  // Receive POINTER from queue
    
    ESP_LOGI(TAG, "Config handler task started");
    
    while (config_handler_running) {
        // Wait for command pointer from gateway
        if (xQueueReceive(g_config_handler_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cmd == NULL) {
                ESP_LOGE(TAG, "Received NULL command pointer!");
                continue;
            }
            
            ESP_LOGI(TAG, "Received config command, type: %d, len: %d", cmd->type, cmd->data_len);
            
            // Route to appropriate handler based on type
            switch (cmd->type) {
                case CONFIG_TYPE_WIFI: {
                    wifi_config_data_t wifi_cfg;
                    if (config_parse_wifi(cmd->raw_data, cmd->data_len, &wifi_cfg) == ESP_OK) {
                        // CRITICAL: Save to NVS FIRST before sending to queue
                        // This ensures config is persisted even if device restarts immediately
                        strncpy(g_wifi_ctx.ssid, wifi_cfg.ssid, sizeof(g_wifi_ctx.ssid) - 1);
                        g_wifi_ctx.ssid[sizeof(g_wifi_ctx.ssid) - 1] = '\0';
                        strncpy(g_wifi_ctx.pass, wifi_cfg.password, sizeof(g_wifi_ctx.pass) - 1);
                        g_wifi_ctx.pass[sizeof(g_wifi_ctx.pass) - 1] = '\0';
                        strncpy(g_wifi_ctx.username, wifi_cfg.username, sizeof(g_wifi_ctx.username) - 1);
                        g_wifi_ctx.username[sizeof(g_wifi_ctx.username) - 1] = '\0';
                        g_wifi_ctx.auth_mode = wifi_cfg.auth_mode;
                        
                        save_wifi_config_to_nvs();
                        ESP_LOGI(TAG, "WiFi config saved to NVS");
                        
                        // Then send to WiFi task for runtime update
                        if (g_wifi_config_queue) {
                            xQueueSend(g_wifi_config_queue, &wifi_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "WiFi config sent to WiFi task");
                        } else {
                            ESP_LOGW(TAG, "WiFi queue not initialized, config saved to NVS only");
                        }
                    }
                    break;
                }
                
                case CONFIG_TYPE_MQTT: {
                    mqtt_config_data_t mqtt_cfg;
                    if (config_parse_mqtt(cmd->raw_data, cmd->data_len, &mqtt_cfg) == ESP_OK) {
                        // CRITICAL: Save to NVS FIRST before sending to queue
                        strncpy(g_mqtt_ctx.broker_uri, mqtt_cfg.broker_uri, sizeof(g_mqtt_ctx.broker_uri) - 1);
                        g_mqtt_ctx.broker_uri[sizeof(g_mqtt_ctx.broker_uri) - 1] = '\0';
                        strncpy(g_mqtt_ctx.device_token, mqtt_cfg.device_token, sizeof(g_mqtt_ctx.device_token) - 1);
                        g_mqtt_ctx.device_token[sizeof(g_mqtt_ctx.device_token) - 1] = '\0';
                        strncpy(g_mqtt_ctx.subscribe_topic, mqtt_cfg.subscribe_topic, sizeof(g_mqtt_ctx.subscribe_topic) - 1);
                        g_mqtt_ctx.subscribe_topic[sizeof(g_mqtt_ctx.subscribe_topic) - 1] = '\0';
                        strncpy(g_mqtt_ctx.publish_topic, mqtt_cfg.publish_topic, sizeof(g_mqtt_ctx.publish_topic) - 1);
                        g_mqtt_ctx.publish_topic[sizeof(g_mqtt_ctx.publish_topic) - 1] = '\0';
                        strncpy(g_mqtt_ctx.attribute_topic, mqtt_cfg.attribute_topic, sizeof(g_mqtt_ctx.attribute_topic) - 1);
                        g_mqtt_ctx.attribute_topic[sizeof(g_mqtt_ctx.attribute_topic) - 1] = '\0';
                        
                        save_mqtt_config_to_nvs();
                        ESP_LOGI(TAG, "MQTT config saved to NVS");
                        
                        // Then send to MQTT task for runtime update
                        if (g_mqtt_config_queue) {
                            xQueueSend(g_mqtt_config_queue, &mqtt_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "MQTT config sent to MQTT task");
                        } else {
                            ESP_LOGW(TAG, "MQTT queue not initialized, config saved to NVS only");
                        }
                    }
                    break;
                }
                case CONFIG_UPDATE_FIRMWARE: {
                    ESP_LOGI(TAG, "Firmware update command received");
                    led_show_blue();
                    mqtt_handler_task_stop(); // Stop MQTT task if running
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    fota_handler_task_start();
                    break;
                }
                case CONFIG_TYPE_INTERNET: {
                    config_internet_type_t internet_type;
                    if (config_parse_internet(cmd->raw_data, cmd->data_len, &internet_type) == ESP_OK) {
                        ESP_LOGI(TAG, "Internet config type parsed: %d", internet_type);
                        g_internet_type = internet_type;  
                        save_internet_config_to_nvs();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    }
                    break;
                }
                case CONFIG_TYPE_LTE: {
                    lte_config_data_t lte_cfg;
                    if (config_parse_lte(cmd->raw_data, cmd->data_len, &lte_cfg) == ESP_OK) {
                        // CRITICAL: Save to NVS FIRST before sending to queue
                        strncpy(g_lte_ctx.apn, lte_cfg.apn, sizeof(g_lte_ctx.apn) - 1);
                        g_lte_ctx.apn[sizeof(g_lte_ctx.apn) - 1] = '\0';
                        strncpy(g_lte_ctx.username, lte_cfg.username, sizeof(g_lte_ctx.username) - 1);
                        g_lte_ctx.username[sizeof(g_lte_ctx.username) - 1] = '\0';
                        strncpy(g_lte_ctx.password, lte_cfg.password, sizeof(g_lte_ctx.password) - 1);
                        g_lte_ctx.password[sizeof(g_lte_ctx.password) - 1] = '\0';
                        g_lte_ctx.comm_type = lte_cfg.comm_type;
                        g_lte_ctx.auto_reconnect = lte_cfg.auto_reconnect;
                        g_lte_ctx.reconnect_timeout_ms = lte_cfg.reconnect_timeout_ms;
                        g_lte_ctx.max_reconnect_attempts = lte_cfg.max_reconnect_attempts;
                        strncpy(g_lte_ctx.modem_name, lte_cfg.modem_name, sizeof(g_lte_ctx.modem_name) - 1);
                        g_lte_ctx.modem_name[sizeof(g_lte_ctx.modem_name) - 1] = '\0';
                        g_lte_ctx.pwr_pin = lte_cfg.pwr_pin;
                        g_lte_ctx.rst_pin = lte_cfg.rst_pin;
                        
                        save_lte_config_to_nvs();
                        ESP_LOGI(TAG, "LTE config saved to NVS");
                        
                        // Then send to LTE task for runtime update
                        if (g_lte_config_queue) {
                            xQueueSend(g_lte_config_queue, &lte_cfg, pdMS_TO_TICKS(100));
                            ESP_LOGI(TAG, "LTE config sent to LTE task");
                        } else {
                            ESP_LOGW(TAG, "LTE queue not initialized");
                        }
                    }
                    break;
                }
                case CONFIG_TYPE_MCU_LAN: {
                    bool is_fota = false;
                    ESP_LOGI(TAG, "MCU LAN command received, forwarding to MCU LAN handler");
                    if (cmd->data_len > 5) {
                        // Validate buffer size BEFORE copying
                        uint16_t cmd_payload_len = cmd->data_len - 3;  // Skip "ML:" prefix
                        
                        if (cmd_payload_len > CONFIG_CMD_MAX_LEN) {
                            ESP_LOGE(TAG, "MCU LAN command too large: %d bytes (max %d)", 
                                     cmd_payload_len, CONFIG_CMD_MAX_LEN);
                            break;
                        }
                        
                        // Allocate on heap to avoid stack overflow (4KB struct)
                        mcu_lan_config_data_t *lan_cmd = (mcu_lan_config_data_t *)malloc(sizeof(mcu_lan_config_data_t));
                        if (lan_cmd == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate MCU LAN command buffer (%d bytes)", 
                                     sizeof(mcu_lan_config_data_t));
                            break;
                        }
                        
                        lan_cmd->length = cmd_payload_len;
                        memcpy(lan_cmd->command, &cmd->raw_data[3], lan_cmd->length);
                        lan_cmd->command[lan_cmd->length] = '\0';  // Null-terminate

                        // Check if this is a firmware update command
                        if (lan_cmd->length >= 4 && strncmp(lan_cmd->command, "CFFW", 4) == 0) {
                            g_not_ppp_to_lan = true;
                            if (!ppp_server_is_initialized()) {
                                ppp_server_init();
                                vTaskDelay(pdMS_TO_TICKS(200));
                            }
                            is_fota = true;
                        }
                        
                        // Send to MCU LAN queue (pass source for response routing)
                        mcu_lan_handler_update_config((const uint8_t *)lan_cmd->command, 
                                                      lan_cmd->length, is_fota, cmd->source);
                        
                        // Free allocated memory
                        free(lan_cmd);
                    } else {
                        ESP_LOGW(TAG, "MCU LAN command too short");
                    }
                    break;
                }
                case CONFIG_TYPE_SERVER: {
                    ESP_LOGI(TAG, "Server type config command received");
                    config_server_type_t new_type;
                    if (config_parse_server_type(cmd->raw_data, cmd->data_len, &new_type) == ESP_OK) {
                        if (new_type != g_server_type) {
                            ESP_LOGI(TAG, "Server type changing from %d to %d, restarting handlers", g_server_type, new_type);
                            
                            // Stop current handler
                            switch(g_server_type) {
                                case CONFIG_SERVERTYPE_MQTT:
                                    mqtt_handler_task_stop();
                                    break;
                                case CONFIG_SERVERTYPE_HTTP:
                                    http_handler_task_stop();
                                    break;
                                case CONFIG_SERVERTYPE_COAP:
                                    coap_handler_task_stop();
                                    break;
                                default:
                                    break;
                            }
                            
                            // Update and save
                            g_server_type = new_type;
                            save_server_config_to_nvs();
                            
                            // Wait for handler to fully stop
                            vTaskDelay(pdMS_TO_TICKS(500));
                            
                            // Start new handler
                            switch(g_server_type) {
                                case CONFIG_SERVERTYPE_MQTT:
                                    mqtt_handler_task_start();
                                    ESP_LOGI(TAG, "MQTT handler started");
                                    break;
                                case CONFIG_SERVERTYPE_HTTP:
                                    http_handler_task_start();
                                    ESP_LOGI(TAG, "HTTP handler started");
                                    break;
                                case CONFIG_SERVERTYPE_COAP:
                                    coap_handler_task_start();
                                    ESP_LOGI(TAG, "CoAP handler started");
                                    break;
                                default:
                                    mqtt_handler_task_start(); // Fallback to MQTT
                                    ESP_LOGW(TAG, "Unknown server type, defaulting to MQTT");
                                    break;
                            }
                            
                            ESP_LOGI(TAG, "Server type updated to: %d", g_server_type);
                        } else {
                            ESP_LOGI(TAG, "Server type already set to %d, no change", g_server_type);
                        }
                    }
                    break;
                }
                case CONFIG_TYPE_HTTP: {
                    ESP_LOGI(TAG, "HTTP config command received");
                    http_config_data_t http_cfg;
                    if (config_parse_http(cmd->raw_data, cmd->data_len, &http_cfg) == ESP_OK) {
                        memcpy(&g_http_cfg, &http_cfg, sizeof(http_config_data_t));
                        save_http_config_to_nvs();
                        ESP_LOGI(TAG, "HTTP config saved - URL: %s", g_http_cfg.server_url);
                        http_handler_update_config(&g_http_cfg);
                    }
                    break;
                }
                case CONFIG_TYPE_COAP: {
                    ESP_LOGI(TAG, "CoAP config command received");
                    coap_config_data_t coap_cfg;
                    if (config_parse_coap(cmd->raw_data, cmd->data_len, &coap_cfg) == ESP_OK) {
                        memcpy(&g_coap_cfg, &coap_cfg, sizeof(coap_config_data_t));
                        save_coap_config_to_nvs();
                        ESP_LOGI(TAG, "CoAP config saved - Host: %s", g_coap_cfg.host);
                        coap_handler_update_config(&g_coap_cfg);
                    }
                    break;
                }
                default:
                    ESP_LOGW(TAG, "Unknown config type: %d", cmd->type);
                    break;
            }
            
            // CRITICAL: Free heap memory after processing
            free(cmd);
            cmd = NULL;
        }
    }
    
    ESP_LOGI(TAG, "Config handler task exiting");
    vTaskDelete(NULL);
}

/**
 * @brief Start config handler task and initialize queues
 */
void config_handler_task_start(void) {
    if (config_handler_running) {
        ESP_LOGW(TAG, "Config handler already running");
        return;
    }
    
    // Create queues if not exists
    if (!g_config_handler_queue) {
        // Store POINTERS to config_command_t (not full 4KB struct!)
        g_config_handler_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(config_command_t*));
    }
    
    if (!g_wifi_config_queue) {
        g_wifi_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(wifi_config_data_t));
    }
    
    if (!g_mqtt_config_queue) {
        g_mqtt_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(mqtt_config_data_t));
    }

    if (!g_lte_config_queue) {
        g_lte_config_queue = xQueueCreate(CONFIG_QUEUE_SIZE, sizeof(lte_config_data_t));
    }
    
    // Create config context mutex for thread-safe access
    if (!g_config_context_mutex) {
        g_config_context_mutex = xSemaphoreCreateMutex();
        if (g_config_context_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create config context mutex");
            return;
        }
        ESP_LOGI(TAG, "Config context mutex created");
    }
    
    config_handler_running = true;
    xTaskCreate(config_handler_task, "config_handler", 4096, NULL, 5, &config_handler_task_handle);
    ESP_LOGI(TAG, "Config handler task created");
}

/**
 * @brief Stop config handler task
 */
void config_handler_task_stop(void) {
    config_handler_running = false;
    
    // Delete mutex
    if (g_config_context_mutex != NULL) {
        vSemaphoreDelete(g_config_context_mutex);
        g_config_context_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "Config handler task stopped");
}
