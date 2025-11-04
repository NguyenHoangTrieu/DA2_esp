#ifndef ESP_MODEM_UART_DTE_H
#define ESP_MODEM_UART_DTE_H

#include "esp_types.h"
#include "esp_err.h"
#include "esp_event.h"

typedef struct modem_dte modem_dte_t;
typedef struct modem_dce modem_dce_t;

/**
 * @brief Working mode of Modem
 *
 */
typedef enum {
    MODEM_COMMAND_MODE = 0, /*!< Command Mode */
    MODEM_PPP_MODE,         /*!< PPP Mode */
    MODEM_CMUX_MODE
} modem_mode_t;

/**
 * @brief Modem flow control type
 *
 */
typedef enum {
    MODEM_FLOW_CONTROL_NONE = 0,
    MODEM_FLOW_CONTROL_SW,
    MODEM_FLOW_CONTROL_HW
} modem_flow_ctrl_t;

/**
 * @brief DTE(Data Terminal Equipment)
 *
 */
struct modem_dte {
    modem_flow_ctrl_t flow_ctrl;                                                    /*!< Flow control of DTE */
    modem_dce_t *dce;                                                               /*!< DCE which connected to the DTE */
    esp_err_t (*send_cmd)(modem_dte_t *dte, const char *command, uint32_t timeout); /*!< Send command to DCE */
    esp_err_t (*send_cmux_cmd)(modem_dte_t *dte, const char *command, uint32_t timeout); /*!< Send command to DCE */
    esp_err_t (*send_sabm)(modem_dte_t *dte, const uint8_t dlci, uint32_t timeout); /*!< Send command to DCE */
    int (*send_data)(modem_dte_t *dte, const char *data, uint32_t length);          /*!< Send data to DCE */
    int (*send_cmux_data)(modem_dte_t *dte, const char *data, uint32_t length);          /*!< Send data to DCE */
    esp_err_t (*send_wait)(modem_dte_t *dte, const char *data, uint32_t length,
                           const char *prompt, uint32_t timeout);      /*!< Wait for specific prompt */
    esp_err_t (*change_mode)(modem_dte_t *dte, modem_mode_t new_mode); /*!< Changing working mode */
    esp_err_t (*process_cmd_done)(modem_dte_t *dte);                   /*!< Callback when DCE process command done */
    esp_err_t (*deinit)(modem_dte_t *dte);                             /*!< Deinitialize */
    bool cmux;
};

#endif