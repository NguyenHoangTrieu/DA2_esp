idf_component_register(SRCS "DA2_esp.c" 
                                # "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_connect.c"
                                # "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_scan.c"
                                # "../Application/Server_Communication_Handler/mqtt_handler/src/mqtt_handler.c"
                                "../BSP/rbg_led_ws_handler/rbg_handler.c" 
                                "../BSP/usb_communication/src/usb_host_task.c"
                                "../BSP/usb_communication/src/rw_task.c"
                                "../BSP/usb_communication/src/jtag_task.c"
                    PRIV_REQUIRES 
                                esp_wifi 
                                nvs_flash
                                mqtt 
                                esp_adc
                                esp_driver_i2c
                                esp_driver_uart
                                esp_driver_gpio 
                                esp_driver_rmt
                                esp_driver_spi
                                usb
                                mbedtls
                                esp_driver_usb_serial_jtag
                    INCLUDE_DIRS "." 
                                # "../Application/Internet_Communication_Handler/wifi_handler/include" 
                                # "../Application/Server_Communication_Handler/mqtt_handler/include"
                                "../BSP/rbg_led_ws_handler"
                                "../BSP/usb_communication/include")
