#ifndef ESP_MODEM_USB_DTE_INTERNAL_H_
#define ESP_MODEM_USB_DTE_INTERNAL_H_

#include "esp_log.h"
#include "esp_modem_usb.h"
#include "esp_modem_usb_dte.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_modem_usb_config.h"

/**
 * @brief Main lifecycle states of the esp-modem
 *
 * The bits below are used to indicate events in process_group
 * field of the esp_modem_usb_dte_internal_t
 */
#define ESP_MODEM_USB_START_BIT     BIT0
#define ESP_MODEM_USB_COMMAND_BIT   BIT1
#define ESP_MODEM_USB_STOP_PPP_BIT  BIT2
#define ESP_MODEM_USB_STOP_BIT      BIT3

/**
 * @brief ESP32 Modem DTE
 *
 */
typedef struct {
    uart_port_t uart_port;                  /*!< UART port */
    uint8_t *buffer;                        /*!< Internal buffer to store response lines/data from DCE */
    uint8_t *data_buffer;                   /*!< Internal buffer to store response data from DCE */
    QueueHandle_t event_queue;              /*!< UART event queue handle */
    esp_event_loop_handle_t event_loop_hdl; /*!< Event loop handle */
    TaskHandle_t uart_event_task_hdl;       /*!< UART event task handle */
    EventGroupHandle_t process_group;       /*!< Event group used for indicating DTE state changes */
    esp_modem_usb_dte_t parent;                 /*!< DTE interface that should extend */
    esp_modem_usb_on_receive receive_cb;        /*!< ptr to data reception */
    void *receive_cb_ctx;                   /*!< ptr to rx fn context data */
    int line_buffer_size;                   /*!< line buffer size in command mode */
    int data_buffer_size;                   /*!< data buffer size in data mode */
    int pattern_queue_size;                 /*!< UART pattern queue size */
    int conn_state;                         /*!< DTE connection state, 0 if disconnect, 1 if connect */
    usbh_cdc_event_cb_t conn_callback;            /*!< DTE connect callback */
    usbh_cdc_event_cb_t disconn_callback;         /*!< DTE disconnect callback */
    usbh_cdc_handle_t cdc_hdl;              /*!< USB CDC handle */
    usbh_cdc_handle_t cdc_hdl2;              /*!< USB CDC handle */
} esp_modem_usb_dte_internal_t;

#endif