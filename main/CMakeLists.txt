idf_component_register(SRCS "DA2_esp.c" 
                                # "../wifi_handler/wifi_connect.c"
                                # "../wifi_handler/wifi_scan.c"
                                # "../mqtt_handler/mqtt_handler.c"
                                "../rbg_led_ws_handler/rbg_handler.c" 
                                "../usb_handler/usb_host_task.c"
                                "../usb_handler/rw_task.c"
                    PRIV_REQUIRES 
                                esp_wifi 
                                nvs_flash
                                mqtt 
                                esp_adc 
                                esp_driver_i2c
                                esp_driver_uart
                                esp_driver_gpio 
                                esp_driver_rmt 
                                usb
                                mbedtls
                                esp_driver_usb_serial_jtag
                    INCLUDE_DIRS "." 
                                # "../wifi_handler" 
                                # "../mqtt_handler"
                                "../rbg_led_ws_handler"
                                "../usb_handler")
