idf_component_register(SRCS "DA2_esp.c" 
                                "../wifi_handler/wifi_connect.c"
                                "../wifi_handler/wifi_scan.c"
                                "../mqtt_handler/mqtt_handler.c"
                                "../sensor_handler/sensor_handler.c"
                    PRIV_REQUIRES esp_wifi nvs_flash mqtt esp_adc
                    INCLUDE_DIRS "." "../wifi_handler" "../mqtt_handler" "../sensor_handler")
