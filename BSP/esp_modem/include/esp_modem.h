#ifndef _ESP_MODEM_H_
#define _ESP_MODEM_H_

#include "esp_modem_dce.h"
#include "esp_modem_dte.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "esp_modem_compat.h"
#include "esp_modem_config.h"

/**
 * @brief Declare Event Base for ESP Modem
 *
 */
ESP_EVENT_DECLARE_BASE(ESP_MODEM_EVENT);

/**
 * @brief ESP Modem Event
 *
 */
typedef enum {
    ESP_MODEM_EVENT_PPP_START = 0,       /*!< ESP Modem Start PPP Session */
    ESP_MODEM_EVENT_PPP_STOP  = 3,       /*!< ESP Modem Stop PPP Session*/
    ESP_MODEM_EVENT_UNKNOWN   = 4        /*!< ESP Modem Unknown Response */
} esp_modem_event_t;

/**
 * @brief ESP Modem DTE Configuration
 *
 */
typedef struct {
    uart_port_t port_num;           /*!< UART port number */
    uart_word_length_t data_bits;   /*!< Data bits of UART */
    uart_stop_bits_t stop_bits;     /*!< Stop bits of UART */
    uart_parity_t parity;           /*!< Parity type */
    modem_flow_ctrl_t flow_control; /*!< Flow control type */
    uint32_t baud_rate;             /*!< Communication baud rate */
    int tx_io_num;                  /*!< TXD Pin Number */
    int rx_io_num;                  /*!< RXD Pin Number */
    int rts_io_num;                 /*!< RTS Pin Number */
    int cts_io_num;                 /*!< CTS Pin Number */
    int rx_buffer_size;             /*!< UART RX Buffer Size */
    int tx_buffer_size;             /*!< UART TX Buffer Size */
    int pattern_queue_size;         /*!< UART Pattern Queue Size */
    int event_queue_size;           /*!< UART Event Queue Size */
    uint32_t event_task_stack_size; /*!< UART Event Task Stack size */
    int event_task_priority;        /*!< UART Event Task Priority */
    int line_buffer_size;           /*!< Line buffer size for command mode */
    bool cmux;
} esp_modem_dte_config_t;

/**
 * @brief Type used for reception callback
 *
 */
typedef esp_err_t (*esp_modem_on_receive)(void *buffer, size_t len, void *context);

/**
 * @brief ESP Modem DTE Default Configuration
 *
 */
#define ESP_MODEM_DTE_DEFAULT_CONFIG()                                          \
    {                                                                           \
        .port_num = ESP_MODEM_CONFIG_UART_PORT_NUM,                             \
        .data_bits = ESP_MODEM_CONFIG_UART_DATA_BITS,                           \
        .stop_bits = ESP_MODEM_CONFIG_UART_STOP_BITS,                           \
        .parity = ESP_MODEM_CONFIG_UART_PARITY,                                 \
        .baud_rate = ESP_MODEM_CONFIG_UART_BAUD_RATE,                           \
        .flow_control = ESP_MODEM_CONFIG_UART_FLOW_CONTROL,                     \
        .tx_io_num = ESP_MODEM_CONFIG_UART_TX_PIN,                              \
        .rx_io_num = ESP_MODEM_CONFIG_UART_RX_PIN,                              \
        .rts_io_num = ESP_MODEM_CONFIG_UART_RTS_PIN,                            \
        .cts_io_num = ESP_MODEM_CONFIG_UART_CTS_PIN,                            \
        .rx_buffer_size = ESP_MODEM_CONFIG_UART_RX_BUFFER_SIZE,                 \
        .tx_buffer_size = ESP_MODEM_CONFIG_UART_TX_BUFFER_SIZE,                 \
        .pattern_queue_size = ESP_MODEM_CONFIG_UART_PATTERN_QUEUE_SIZE,         \
        .event_queue_size = ESP_MODEM_CONFIG_UART_EVENT_QUEUE_SIZE,             \
        .event_task_stack_size = ESP_MODEM_CONFIG_UART_EVENT_TASK_STACK_SIZE,   \
        .event_task_priority = ESP_MODEM_CONFIG_UART_EVENT_TASK_PRIORITY,       \
        .line_buffer_size = 512,                                                \
        .cmux = true                                                            \
    }

/**
 * @brief Create and initialize Modem DTE object
 *
 * @param config configuration of ESP Modem DTE object
 * @return modem_dte_t*
 *      - Modem DTE object
 */
modem_dte_t *esp_modem_dte_init(const esp_modem_dte_config_t *config);

/**
 * @brief Register event handler for ESP Modem event loop
 *
 * @param handler event handler to register
 * @param event_id event id to register, use ESP_EVENT_ANY_ID to register for all events
 * @param handler_args arguments for registered handler
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM on allocating memory for the handler failed
 *      - ESP_ERR_INVALID_ARG on invalid combination of event base and event id
 */
esp_err_t esp_modem_set_event_handler(esp_event_handler_t handler, int32_t event_id, void *handler_args);

/**
 * @brief Unregister event handler for ESP Modem event loop
 *
 * @param handler event handler to unregister
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on invalid combination of event base and event id
 */
esp_err_t esp_modem_remove_event_handler(esp_event_handler_t handler);

/**
 * @brief Setup PPP Session
 *
 * @param dte Modem DTE object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
esp_err_t esp_modem_start_ppp(modem_dte_t *dte);

/**
 * @brief Start CMUX
 *
 * @param dte Modem DTE object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
esp_err_t esp_modem_start_cmux(modem_dte_t *dte);

/**
 * @brief Exit PPP Session
 *
 * @param dte Modem DTE Object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
esp_err_t esp_modem_stop_ppp(modem_dte_t *dte);

/**
 * @brief Setup on reception callback
 *
 * @param dte ESP Modem DTE object
 * @param receive_cb Function pointer to the reception callback
 * @param receive_cb_ctx Contextual pointer to be passed to the reception callback
 *
 * @return ESP_OK on success
 */
esp_err_t esp_modem_set_rx_cb(modem_dte_t *dte, esp_modem_on_receive receive_cb, void *receive_cb_ctx);

#endif // _ESP_MODEM_H_