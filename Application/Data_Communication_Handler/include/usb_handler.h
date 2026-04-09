#ifndef USB_HANDLER_H
#define USB_HANDLER_H

#include "config_handler.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "sdkconfig.h"
#include "esp_check.h"

#define JTAG_TASK_PRIORITY      3

// JTAG task control functions
void jtag_task_start(void);
void jtag_task_stop(void);

#endif // USB_HANDLER_H