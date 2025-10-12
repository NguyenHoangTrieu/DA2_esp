idf_component_register(SRCS "DA2_esp.c" 
                            "../wifi_handler/wifi_connect.c"
                            "../wifi_handler/wifi_scan.c"
                            "../mqtt_handler/mqtt_handle.c"
                    PRIV_REQUIRES esp_wifi nvs_flash
                    INCLUDE_DIRS "." "../wifi_handler" "../mqtt_handler")
