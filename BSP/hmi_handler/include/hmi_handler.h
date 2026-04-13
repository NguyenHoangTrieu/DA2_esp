/**
 * @file hmi_handler.h
 * @brief HMI BSP — Raw UART driver for TJC3224T024_011 (2.4" 240×320 USART HMI)
 *
 * BSP layer: UART2 driver install/uninstall and raw byte-level read/write.
 * Higher-level TJC protocol and page logic live in hmi_display.h (Middleware).
 * FreeRTOS task management lives in hmi_task.h (Application).
 *
 * Hardware:
 *   UART2  TX=GPIO41, RX=GPIO42, 115200 baud
 *   UART_SEL = GPIO46: 0 = LAN MCU path   1 = HMI LCD path  (FSUSB42UMX-TP switch)
 */

#ifndef HMI_HANDLER_H
#define HMI_HANDLER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Hardware constants                                                  */
/* ------------------------------------------------------------------ */

#define HMI_BSP_UART_NUM    2        /* UART_NUM_2                        */
#define HMI_BSP_TX_PIN      41       /* Shared UART2 TX (PPP / LAN MCU)  */
#define HMI_BSP_RX_PIN      42       /* Shared UART2 RX                  */
#define HMI_BSP_BAUD        115200
#define HMI_BSP_BUF_SIZE    1024     /* UART ring-buffer size in bytes    */

/* TJC protocol: every command ends with 3 × 0xFF */
#define HMI_TJC_TERM        "\xFF\xFF\xFF"
#define HMI_TJC_TERM_LEN    3

/* ------------------------------------------------------------------ */
/*  Public API — raw UART functions only                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Install UART2 driver at HMI_BSP_BAUD.
 *        Must be called AFTER the UART switch is routed to HMI (GPIO46=HIGH).
 * @return ESP_OK on success.
 */
esp_err_t hmi_bsp_init(void);

/**
 * @brief Uninstall UART2 driver and release all resources.
 */
void hmi_bsp_deinit(void);

/**
 * @brief Write raw bytes to UART2.
 * @param data  Pointer to data buffer.
 * @param len   Number of bytes to write.
 */
void hmi_bsp_write(const uint8_t *data, size_t len);

/**
 * @brief Block-read bytes from UART2 until 3 consecutive 0xFF bytes are
 *        received (TJC frame terminator) or timeout expires.
 * @param buf         Destination buffer (frame content including terminators).
 * @param buf_size    Capacity of buf in bytes.
 * @param timeout_ms  Maximum wait time in milliseconds.
 * @return Number of bytes placed in buf; 0 on timeout.
 */
int hmi_bsp_read_frame(uint8_t *buf, size_t buf_size, uint32_t timeout_ms);

/**
 * @brief Flush the UART2 hardware RX buffer.
 */
void hmi_bsp_drain(void);

/* ------------------------------------------------------------------ */
/*  NOTE: hmi_status_t, page IDs, color constants and all higher-level */
/*        APIs are in hmi_display.h (Middleware) and hmi_task.h (App). */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
}
#endif
#endif /* HMI_HANDLER_H */
