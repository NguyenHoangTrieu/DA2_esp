file(GLOB MODEM_UART_SRCS "../BSP/esp_modem_uart/src/*.c")
file(GLOB MODEM_USB_SRCS "../BSP/esp_modem_usb/src/*.c")

idf_component_register(SRCS "DA2_esp.c" 
                                "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_connect.c"
                                "../Application/Internet_Communication_Handler/lte_handler/src/lte_connect.c"
                                "../Application/Internet_Communication_Handler/wifi_handler/src/wifi_scan.c"
                                "../Application/Server_Communication_Handler/mqtt_handler/src/mqtt_handler.c"
                                "../Application/Config_Handler/src/config_handler.c"
                                "../Application/FOTA/src/fota_handler.c"
                                "../Application/PPP_Server/src/ppp_server.c"
                                "../Application/Data_Communication_Handler/src/usb_handler.c"
                                "../Application/Data_Communication_Handler/src/uart_handler.c"
                                "../Application/MCU_LAN_Handler/src/mcu_lan_handler.c"
                                "../BSP/rbg_led_ws_handler/rbg_handler.c" 
                                "../BSP/usb_communication/src/usb_host_function.c"
                                ${MODEM_UART_SRCS}
                                ${MODEM_USB_SRCS}
                                "../BSP/bg96_communication/src/bg96_comm.c"
                                "../BSP/sim7600_communication/src/sim7600_comm.c"
                                "../BSP/MCU_LAN_Communication/src/lan_comm.c"
                                "../Middleware/LTE_Handler/src/lte_handler.c"
                    PRIV_REQUIRES 
                                esp_wifi
                                nvs_flash
                                mqtt
                                esp_adc
                                esp_driver_i2c
                                esp_driver_uart
                                esp_driver_gpio
                                esp_driver_spi
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
                                esp_ringbuf
                    INCLUDE_DIRS "." 
                                "../Application/Internet_Communication_Handler/wifi_handler/include"
                                "../Application/Internet_Communication_Handler/lte_handler/include"
                                "../Application/Server_Communication_Handler/mqtt_handler/include"
                                "../Application/Config_Handler/include"
                                "../Application/FOTA/include"
                                "../Application/PPP_Server/include"
                                "../Application/Data_Communication_Handler/include"
                                "../Application/MCU_LAN_Handler/include"
                                "../BSP/rbg_led_ws_handler"
                                "../BSP/usb_communication/include"
                                "../BSP/esp_modem_uart/include"
                                "../BSP/esp_modem_usb/include"
                                "../BSP/bg96_communication/include"
                                "../BSP/sim7600_communication/include"
                                "../BSP/MCU_LAN_Communication/include"
                                "../Middleware/LTE_Handler/include"
                    EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
