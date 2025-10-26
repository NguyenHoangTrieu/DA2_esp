#ifndef _LTE_HANDLER_H_
#define _LTE_HANDLER_H_

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LTE Handler Event Base
 */
ESP_EVENT_DECLARE_BASE(LTE_HANDLER_EVENT);

/**
 * @brief LTE Handler Events
 */
typedef enum {
    LTE_EVENT_INITIALIZED = 0,      /*!< Handler initialized */
    LTE_EVENT_MODEM_READY,          /*!< Modem ready and registered */
    LTE_EVENT_CONNECTING,           /*!< Connecting to network */
    LTE_EVENT_CONNECTED,            /*!< Connected to Internet (Got IP) */
    LTE_EVENT_DISCONNECTED,         /*!< Disconnected from Internet */
    LTE_EVENT_ERROR,                /*!< Error occurred */
    LTE_EVENT_RECONNECTING,         /*!< Auto-reconnecting */
} lte_handler_event_t;

/**
 * @brief LTE Handler State
 */
typedef enum {
    LTE_STATE_IDLE = 0,             /*!< Not initialized */
    LTE_STATE_INITIALIZING,         /*!< Initializing modem */
    LTE_STATE_INITIALIZED,          /*!< Modem initialized */
    LTE_STATE_REGISTERING,          /*!< Registering to network */
    LTE_STATE_REGISTERED,           /*!< Registered to network */
    LTE_STATE_CONNECTING,           /*!< Connecting PPP */
    LTE_STATE_RECONNECTING,         /*!< Reconnecting (auto-reconnect) */
    LTE_STATE_CONNECTED,            /*!< Connected (has IP) */
    LTE_STATE_DISCONNECTED,         /*!< Disconnected */
    LTE_STATE_ERROR,                /*!< Error state */
} lte_handler_state_t;

/**
 * @brief LTE Network Information
 */
typedef struct {
    char ip[16];                    /*!< IP address */
    char netmask[16];               /*!< Netmask */
    char gateway[16];               /*!< Gateway */
    char dns1[16];                  /*!< Primary DNS */
    char dns2[16];                  /*!< Secondary DNS */
} lte_network_info_t;

/**
 * @brief LTE Modem Information
 */
typedef struct {
    char imei[16];                  /*!< IMEI number */
    char imsi[16];                  /*!< IMSI number */
    char module_name[32];           /*!< Module name */
    char operator_name[32];         /*!< Operator name */
    uint32_t rssi;                  /*!< Signal strength (RSSI) */
    uint32_t ber;                   /*!< Bit error rate */
} lte_modem_info_t;

/**
 * @brief LTE Handler Configuration
 */
typedef struct {
    const char *apn;                /*!< Access Point Name (required) */
    const char *username;           /*!< PPP auth username (optional) */
    const char *password;           /*!< PPP auth password (optional) */
    bool auto_reconnect;            /*!< Enable auto-reconnect */
    uint32_t reconnect_timeout_ms;  /*!< Reconnect retry timeout (default: 30000ms) */
    uint32_t max_reconnect_attempts;/*!< Max reconnect attempts (0 = infinite) */
} lte_handler_config_t;

/**
 * @brief Default LTE Handler Configuration
 */
#define LTE_HANDLER_CONFIG_DEFAULT() {          \
    .apn = "internet",                          \
    .username = NULL,                           \
    .password = NULL,                           \
    .auto_reconnect = true,                     \
    .reconnect_timeout_ms = 30000,              \
    .max_reconnect_attempts = 0,                \
}

/**
 * @brief Initialize LTE Handler
 * 
 * @param config Configuration parameters
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL or APN is NULL
 *      - ESP_ERR_NO_MEM if memory allocation failed
 *      - ESP_FAIL if initialization failed
 */
esp_err_t lte_handler_init(const lte_handler_config_t *config);

/**
 * @brief Deinitialize LTE Handler
 * 
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t lte_handler_deinit(void);

/**
 * @brief Connect to LTE network
 * 
 * This function initiates PPP connection. It's non-blocking and returns immediately.
 * Connection status will be notified via LTE_EVENT_CONNECTED event.
 * 
 * @return 
 *      - ESP_OK on success (connection initiated)
 *      - ESP_ERR_INVALID_STATE if not initialized or already connected
 *      - ESP_FAIL if connection failed to start
 */
esp_err_t lte_handler_connect(void);

/**
 * @brief Disconnect from LTE network
 * 
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t lte_handler_disconnect(void);

/**
 * @brief Get current connection state
 * 
 * @return Current LTE handler state
 */
lte_handler_state_t lte_handler_get_state(void);

/**
 * @brief Check if connected to Internet
 * 
 * @return 
 *      - true if connected (has IP address)
 *      - false otherwise
 */
bool lte_handler_is_connected(void);

/**
 * @brief Get signal strength (CSQ)
 * 
 * @param[out] rssi Received Signal Strength Indicator (0-31, 99=unknown)
 * @param[out] ber Bit Error Rate (0-7, 99=unknown)
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if parameters are NULL
 *      - ESP_ERR_INVALID_STATE if modem not ready
 */
esp_err_t lte_handler_get_signal_strength(uint32_t *rssi, uint32_t *ber);

/**
 * @brief Get operator name
 * 
 * @param[out] operator_name Buffer to store operator name
 * @param[in] max_len Maximum buffer length
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if parameters are NULL
 *      - ESP_ERR_INVALID_STATE if modem not ready
 */
esp_err_t lte_handler_get_operator_name(char *operator_name, size_t max_len);

/**
 * @brief Get IP information
 * 
 * @param[out] info Network information structure
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if info is NULL
 *      - ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t lte_handler_get_ip_info(lte_network_info_t *info);

/**
 * @brief Get IMEI number
 * 
 * @param[out] imei Buffer to store IMEI (minimum 16 bytes)
 * @param[in] max_len Maximum buffer length
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if parameters are NULL
 *      - ESP_ERR_INVALID_STATE if modem not ready
 */
esp_err_t lte_handler_get_imei(char *imei, size_t max_len);

/**
 * @brief Get IMSI number
 * 
 * @param[out] imsi Buffer to store IMSI (minimum 16 bytes)
 * @param[in] max_len Maximum buffer length
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if parameters are NULL
 *      - ESP_ERR_INVALID_STATE if modem not ready
 */
esp_err_t lte_handler_get_imsi(char *imsi, size_t max_len);

/**
 * @brief Get complete modem information
 * 
 * @param[out] info Modem information structure
 * @return 
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if info is NULL
 *      - ESP_ERR_INVALID_STATE if modem not ready
 */
esp_err_t lte_handler_get_modem_info(lte_modem_info_t *info);

/**
 * @brief Get esp_netif handle
 * 
 * This allows upper layers to use standard socket API
 * 
 * @return 
 *      - esp_netif handle if initialized
 *      - NULL if not initialized
 */
esp_netif_t *lte_handler_get_netif(void);

/**
 * @brief Enable/Disable auto-reconnect
 * 
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t lte_handler_set_auto_reconnect(bool enable);

/**
 * @brief Set reconnect parameters
 * 
 * @param timeout_ms Timeout between reconnect attempts (milliseconds)
 * @param max_attempts Maximum reconnect attempts (0 = infinite)
 * @return ESP_OK on success
 */
esp_err_t lte_handler_set_reconnect_params(uint32_t timeout_ms, uint32_t max_attempts);

#ifdef __cplusplus
}
#endif

#endif /* _LTE_HANDLER_H_ */
