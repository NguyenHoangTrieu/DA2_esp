#ifndef DA2_ESP_H
#define DA2_ESP_H

#include "usb_handler.h"
#include "rbg_handler.h"
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
#include "fota_ap.h"
#include "web_config_handler.h"
#include "i2c_dev_support.h"
#include "oled_monitor_task.h"
#include "tca_handler.h"
#include "stack_handler.h"
#include "pwr_source_handler.h"
#include "pcf8563_rtc.h"
extern TaskHandle_t main_task_handle;
void server_connect_stop(config_server_type_t server_type);
void server_connect_start(config_server_type_t server_type);
#endif // DA2_ESP_H