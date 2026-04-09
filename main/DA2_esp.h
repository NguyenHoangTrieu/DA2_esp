#ifndef DA2_ESP_H
#define DA2_ESP_H

#include "usb_handler.h"

#include "wifi_connect.h"
#include "fota_handler.h"
#include "uart_handler.h"
#include "mqtt_handler.h"
#include "http_handler.h"
#include "coap_handler.h"
#include "config_handler.h"
#include "lte_connect.h"
#include "eth_connect.h"
#include "mcu_lan_handler.h"
#include "ppp_server.h"
#include "web_config_handler.h"
#include "i2c_dev_support.h"
#include "tca_handler.h"
#include "stack_handler.h"
#include "pwr_source_handler.h"
#include "pwr_monitor_task.h"
#include "hmi_handler.h"
#include "pcf8563_rtc.h"
extern TaskHandle_t main_task_handle;
void server_connect_stop(config_server_type_t server_type);
void server_connect_start(config_server_type_t server_type);

/* UART Switch (FSUSB42UMX-TP, GPIO46): routes UART2 to LAN MCU or HMI LCD */
void uart_switch_init(void);
void uart_switch_route_to_lan_mcu(void);
void uart_switch_route_to_hmi(void);

#endif // DA2_ESP_H