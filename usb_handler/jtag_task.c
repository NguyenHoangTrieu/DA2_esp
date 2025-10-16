/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "usb_handler.h"

#define BUF_SIZE (1024)

static TaskHandle_t jtag_task_hdl = NULL;

static void jtag_task(void *arg)
{
    // Configure USB SERIAL JTAG
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
        .rx_buffer_size = BUF_SIZE,
        .tx_buffer_size = BUF_SIZE,
    };

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
    ESP_LOGI("usb_serial_jtag echo", "USB_SERIAL_JTAG init done");

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE("usb_serial_jtag echo", "no memory for data");
        return;
    }

    while (1) {

        int len = usb_serial_jtag_read_bytes(data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);

        // Write data back to the USB SERIAL JTAG
        if (len) {
            usb_serial_jtag_write_bytes((const char *) data, len, 20 / portTICK_PERIOD_MS);
            data[len] = '\0';
            ESP_LOG_BUFFER_HEXDUMP("Recv str: ", data, len, ESP_LOG_INFO);
        }
    }
}

// =============================================================================

void jtag_task_start(void)
{
    BaseType_t task_created;
    // Create jtag task
    task_created = xTaskCreatePinnedToCore(jtag_task,
                                           "usb_serial_jtag_echo",
                                           4096,
                                           NULL,
                                           JTAG_TASK_PRIORITY,
                                           &jtag_task_hdl,
                                           0);
    assert(task_created == pdTRUE);
}

void jtag_task_resume(void)
{
    if (jtag_task_hdl != NULL) {
        vTaskResume(jtag_task_hdl);
    }
}

void jtag_task_stop(void)
{
    if (jtag_task_hdl != NULL) {
        vTaskSuspend(jtag_task_hdl);
    }
}