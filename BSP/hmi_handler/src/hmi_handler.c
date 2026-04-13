/**
 * @file hmi_handler.c
 * @brief HMI BSP — Raw UART driver for TJC3224T024_011 (2.4" 240x320 portrait USART HMI)
 *
 * BSP layer only: UART2 driver install/uninstall and raw byte-level read/write.
 * All higher-level TJC protocol logic lives in hmi_display.c (Middleware).
 *
 * UART2 is shared with the LAN MCU / PPP server path via FSUSB42UMX-TP switch.
 * GPIO46 must be driven HIGH before calling hmi_bsp_init().
 */

#include "hmi_handler.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HMI_BSP";

/* ------------------------------------------------------------------ */
/*  BSP API implementation                                              */
/* ------------------------------------------------------------------ */

esp_err_t hmi_bsp_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = HMI_BSP_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(HMI_BSP_UART_NUM,
                                        HMI_BSP_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uart_param_config(HMI_BSP_UART_NUM, &cfg);
    uart_set_pin(HMI_BSP_UART_NUM,
                 HMI_BSP_TX_PIN, HMI_BSP_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART%d BSP init OK (TX=%d RX=%d %d baud)",
             HMI_BSP_UART_NUM, HMI_BSP_TX_PIN, HMI_BSP_RX_PIN, HMI_BSP_BAUD);
    return ESP_OK;
}

void hmi_bsp_deinit(void)
{
    uart_driver_delete(HMI_BSP_UART_NUM);
    ESP_LOGI(TAG, "UART%d BSP deinit", HMI_BSP_UART_NUM);
}

void hmi_bsp_write(const uint8_t *data, size_t len)
{
    uart_write_bytes(HMI_BSP_UART_NUM, (const char *)data, len);
}

int hmi_bsp_read_frame(uint8_t *buf, size_t buf_size, uint32_t timeout_ms)
{
    int idx      = 0;
    int ff_count = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t byte;
        int n = uart_read_bytes(HMI_BSP_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (n <= 0) continue;

        if (idx < (int)buf_size - 1) {
            buf[idx++] = byte;
        }

        if (byte == 0xFF) {
            if (++ff_count >= 3) return idx;   /* complete TJC frame */
        } else {
            ff_count = 0;
        }
    }
    return 0;   /* timeout */
}

void hmi_bsp_drain(void)
{
    uart_flush_input(HMI_BSP_UART_NUM);
}
