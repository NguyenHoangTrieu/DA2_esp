idf_component_register(SRCS "DA2_esp.c" 
                                "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_connect.c"
                                "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_scan.c"
                                "../Application/Server_Communication_Handler/mqtt_handler/src/mqtt_handler.c"
                                "../Application/Config_Handler/src/config_handler.c"
                                "../BSP/rbg_led_ws_handler/rbg_handler.c" 
                                "../BSP/usb_communication/src/usb_host_function.c"
                                "../BSP/usb_communication/src/rw_function.c"
                                "../Middleware/Data_Communication_Handler/src/usb_handler.c"
                                "../Middleware/Data_Communication_Handler/src/uart_handler.c"
                                "../Application/FOTA/src/fota_handler.c"
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
                                esp_http_client
                                app_update
                                esp_https_ota
                                esp_netif
                                esp_psram
                    INCLUDE_DIRS "." 
                                "../Application/Internet_Communication_Handler/wifi_handler/include" 
                                "../Application/Server_Communication_Handler/mqtt_handler/include"
                                "../Application/Config_Handler/include"
                                "../BSP/rbg_led_ws_handler"
                                "../BSP/usb_communication/include"
                                "../Middleware/Data_Communication_Handler/include"
                                "../Application/FOTA/include"
                    EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
