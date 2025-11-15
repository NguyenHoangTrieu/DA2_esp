/**
 * @file esp_modem_usb_config.h
 * @brief ESP32 USB Host Modem Configuration Header
 * 
 * Auto-generated configuration header from Kconfig
 * This file contains all modem-specific USB configuration parameters
 */

#ifndef ESP_MODEM_USB_CONFIG_H
#define ESP_MODEM_USB_CONFIG_H

#include "sdkconfig.h"

/* ==================== Version Information ==================== */

#ifndef IOT_USBH_MODEM_VER_MAJOR
    #define IOT_USBH_MODEM_VER_MAJOR    1
#endif

#ifndef IOT_USBH_MODEM_VER_MINOR
    #define IOT_USBH_MODEM_VER_MINOR    0
#endif

#ifndef IOT_USBH_MODEM_VER_PATCH
    #define IOT_USBH_MODEM_VER_PATCH    0
#endif

/* CDC Driver Version Information */
#ifndef IOT_USBH_CDC_VER_MAJOR
    #define IOT_USBH_CDC_VER_MAJOR      1
#endif

#ifndef IOT_USBH_CDC_VER_MINOR
    #define IOT_USBH_CDC_VER_MINOR      0
#endif

#ifndef IOT_USBH_CDC_VER_PATCH
    #define IOT_USBH_CDC_VER_PATCH      0
#endif

/* ==================== USB Host Task Configuration ==================== */

#ifndef CONFIG_USBH_TASK_BASE_PRIORITY
    #define CONFIG_USBH_TASK_BASE_PRIORITY  5
#endif

#ifndef CONFIG_USBH_TASK_CORE_ID
    #define CONFIG_USBH_TASK_CORE_ID        1
#endif

#ifndef CONFIG_USBH_TASK_STACK_SIZE
    #define CONFIG_USBH_TASK_STACK_SIZE     1024 * 32
#endif

#ifndef CONFIG_MODEM_DAEMON_STACK_SIZE
    #define CONFIG_MODEM_DAEMON_STACK_SIZE     1024 * 32
#endif

/* ==================== USB Transfer Buffer Configuration ==================== */

#ifndef CONFIG_CTRL_TRANSFER_DATA_MAX_BYTES
    #define CONFIG_CTRL_TRANSFER_DATA_MAX_BYTES  512
#endif

#ifndef CONFIG_CONTROL_TRANSFER_BUFFER_SIZE
    #define CONFIG_CONTROL_TRANSFER_BUFFER_SIZE  512
#endif

#ifndef CONFIG_IN_TRANSFER_BUFFER_SIZE
    #define CONFIG_IN_TRANSFER_BUFFER_SIZE       2048
#endif

#ifndef CONFIG_OUT_TRANSFER_BUFFER_SIZE
    #define CONFIG_OUT_TRANSFER_BUFFER_SIZE      2048
#endif

#ifndef CONFIG_DEVICE_ADDRESS_LIST_NUM
    #define CONFIG_DEVICE_ADDRESS_LIST_NUM       4
#endif


/* ==================== Modem Target Selection ==================== */
#define CONFIG_MODEM_TARGET_A7600C1      1

#if defined(CONFIG_MODEM_TARGET_USER)
    #define CONFIG_MODEM_TARGET_NAME               "User Defined"
#elif defined(CONFIG_MODEM_TARGET_NT26)
    #define  CONFIG_MODEM_TARGET_NAME               "NT26"
#elif defined(CONFIG_MODEM_TARGET_ML302_DNLM)
    #define  CONFIG_MODEM_TARGET_NAME               "ML302-DNLM/CNLM"
#elif defined(CONFIG_MODEM_TARGET_AIR724UG_NFM)
    #define CONFIG_MODEM_TARGET_NAME               "AIR724UG-NFM"
#elif defined(CONFIG_MODEM_TARGET_AIR780_E)
    #define CONFIG_MODEM_TARGET_NAME               "AIR780E"
#elif defined(CONFIG_MODEM_TARGET_EC600N_CNLA_N05)
    #define CONFIG_MODEM_TARGET_NAME               "EC600NCNLA-N05"
#elif defined(CONFIG_MODEM_TARGET_EC600N_CNLC_N06)
    #define CONFIG_MODEM_TARGET_NAME               "EC600NCNLC-N06"
#elif defined(CONFIG_MODEM_TARGET_A7600C1)
    #define CONFIG_MODEM_TARGET_NAME               "A7600C1"
#elif defined(CONFIG_MODEM_TARGET_BG95_M3)
    #define CONFIG_MODEM_TARGET_NAME               "BG95M3"
#elif defined(CONFIG_MODEM_TARGET_BG96_MA)
    #define CONFIG_MODEM_TARGET_NAME               "BG96MA"
#elif defined(CONFIG_MODEM_TARGET_MC610_EU)
    #define CONFIG_MODEM_TARGET_NAME               "MC610_EU"
#elif defined(CONFIG_MODEM_TARGET_EC20_CE)
    #define CONFIG_MODEM_TARGET_NAME               "EC20_CE"
#elif defined(CONFIG_MODEM_TARGET_EG25_GL)
    #define CONFIG_MODEM_TARGET_NAME               "EG25_GL"
#elif defined(CONFIG_MODEM_TARGET_YM310_X09)
    #define CONFIG_MODEM_TARGET_NAME               "YM310_X09"
#elif defined(CONFIG_MODEM_TARGET_SIM7600E)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7600E"
#elif defined(CONFIG_MODEM_TARGET_A7670E)
    #define CONFIG_MODEM_TARGET_NAME               "A7670E"
#elif defined(CONFIG_MODEM_TARGET_SIM7070G)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7070G"
#elif defined(CONFIG_MODEM_TARGET_SIM7080)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7080G"
#else  // Default
    #define CONFIG_MODEM_TARGET_NAME               "ML302-DNLM/CNLM"
#endif

/* ==================== Dual CDC Mode Support ==================== */

#ifdef CONFIG_MODEM_SUPPORT_SECONDARY_AT_PORT
    #define MODEM_SUPPORT_SECONDARY_AT_PORT 1
#else
    #define MODEM_SUPPORT_SECONDARY_AT_PORT 0
#endif

/* ==================== USB CDC Interface Configuration ==================== */
#define MODEM_TARGET_A7600C1
#ifndef CONFIG_MODEM_USB_ITF
    #if defined(MODEM_TARGET_NT26)
        #define CONFIG_MODEM_USB_ITF        0x03
    #elif defined(MODEM_TARGET_ML302_DNLM)
        #define CONFIG_MODEM_USB_ITF        0x00
    #elif defined(MODEM_TARGET_EC600N_CNLC_N06)
        #define CONFIG_MODEM_USB_ITF        0x04
    #elif defined(MODEM_TARGET_MC610_EU)
        #define CONFIG_MODEM_USB_ITF        0x06
    #elif defined(MODEM_TARGET_A7600C1)
        #define CONFIG_MODEM_USB_ITF        0x05
    #elif defined(MODEM_TARGET_EC20_CE)
        #define CONFIG_MODEM_USB_ITF        0x02
    #elif defined(MODEM_TARGET_AIR780_E)
        #define CONFIG_MODEM_USB_ITF        0x03
    #elif defined(MODEM_TARGET_EG25_GL)
        #define CONFIG_MODEM_USB_ITF        0x03
    #elif defined(MODEM_TARGET_YM310_X09)
        #define CONFIG_MODEM_USB_ITF        0x03
    #elif defined(MODEM_TARGET_BG96_MA)
        #define CONFIG_MODEM_USB_ITF        0x02
    #elif defined(MODEM_TARGET_SIM7600E)
        #define CONFIG_MODEM_USB_ITF        0x03
    #elif defined(MODEM_TARGET_A7670E)
        #define CONFIG_MODEM_USB_ITF        0x04
    #elif defined(MODEM_TARGET_SIM7070G)
        #define CONFIG_MODEM_USB_ITF        0x02
    #elif defined(MODEM_TARGET_SIM7080)
        #define CONFIG_MODEM_USB_ITF        0x02
    #else
        #define CONFIG_MODEM_USB_ITF        0x00  // User defined default
    #endif
#endif

#define MODEM_USB_INTERFACE_PRIMARY     CONFIG_MODEM_USB_ITF

/* Secondary USB CDC Interface (for Dual CDC Mode) */
#if MODEM_SUPPORT_SECONDARY_AT_PORT
    #ifndef CONFIG_MODEM_USB_ITF2
        #if defined(MODEM_TARGET_MC610_EU)
            #define CONFIG_MODEM_USB_ITF2   0x05
        #elif defined(MODEM_TARGET_EG25_GL)
            #define CONFIG_MODEM_USB_ITF2   0x02
        #elif defined(MODEM_TARGET_A7670E)
            #define CONFIG_MODEM_USB_ITF2   0x05
        #else
            #define CONFIG_MODEM_USB_ITF2   0x01  // User defined default
        #endif
    #endif
    #define MODEM_USB_INTERFACE_SECONDARY   CONFIG_MODEM_USB_ITF2
#endif

/* ==================== Modem Connection Configuration ==================== */

#ifndef CONFIG_MODEM_DIAL_RETRY_TIMES
    #define CONFIG_MODEM_DIAL_RETRY_TIMES   5
#endif
#define MODEM_DIAL_RETRY_TIMES              CONFIG_MODEM_DIAL_RETRY_TIMES

#ifndef CONFIG_MODEM_PPP_APN
    #define CONFIG_MODEM_PPP_APN            "internet"
#endif
#define MODEM_APN                           CONFIG_MODEM_PPP_APN

#ifndef CONFIG_MODEM_SIM_PIN_PWD
    #define CONFIG_MODEM_SIM_PIN_PWD        "1234"
#endif
#define MODEM_SIM_PIN                       CONFIG_MODEM_SIM_PIN_PWD

#ifndef CONFIG_MODEM_PRINT_DEVICE_DESCRIPTOR
    #define CONFIG_MODEM_PRINT_DEVICE_DESCRIPTOR 1
#endif
#define MODEM_PRINT_DEVICE_DESC             CONFIG_MODEM_PRINT_DEVICE_DESCRIPTOR

/* ==================== GPIO Configuration ==================== */

#ifndef CONFIG_MODEM_POWER_GPIO
    #define CONFIG_MODEM_POWER_GPIO         22
#endif
#define MODEM_GPIO_POWER                    CONFIG_MODEM_POWER_GPIO

#ifndef CONFIG_MODEM_RESET_GPIO
    #define CONFIG_MODEM_RESET_GPIO         23
#endif
#define MODEM_GPIO_RESET                    CONFIG_MODEM_RESET_GPIO

#ifndef CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL
    #define CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL  1
#endif
#define MODEM_GPIO_POWER_INACTIVE_LEVEL     CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL

#ifndef CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL
    #define CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL  1
#endif
#define MODEM_GPIO_RESET_INACTIVE_LEVEL     CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL

/* ==================== Timeout Configuration (milliseconds) ==================== */

#ifndef CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT
    #define CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT    2000
#endif
#define MODEM_TIMEOUT_DEFAULT_MS            CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT

#ifndef CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR
    #define CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR   6000
#endif
#define MODEM_TIMEOUT_OPERATOR_MS           CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR

#ifndef CONFIG_MODEM_COMMAND_TIMEOUT_RESET
    #define CONFIG_MODEM_COMMAND_TIMEOUT_RESET      6000
#endif
#define MODEM_TIMEOUT_RESET_MS              CONFIG_MODEM_COMMAND_TIMEOUT_RESET

#ifndef CONFIG_MODEM_COMMAND_TIMEOUT_MODE_CHANGE
    #define CONFIG_MODEM_COMMAND_TIMEOUT_MODE_CHANGE    10000
#endif
#define MODEM_TIMEOUT_MODE_CHANGE_MS        CONFIG_MODEM_COMMAND_TIMEOUT_MODE_CHANGE

#ifndef CONFIG_MODEM_COMMAND_TIMEOUT_POWEROFF
    #define CONFIG_MODEM_COMMAND_TIMEOUT_POWEROFF   1000
#endif
#define MODEM_TIMEOUT_POWEROFF_MS           CONFIG_MODEM_COMMAND_TIMEOUT_POWEROFF

/* ==================== Wi-Fi SoftAP Configuration ==================== */

#ifndef CONFIG_MODEM_WIFI_SSID
    #define CONFIG_MODEM_WIFI_SSID          "esp_4g_router"
#endif
#define MODEM_WIFI_SSID                     CONFIG_MODEM_WIFI_SSID

#ifndef CONFIG_MODEM_WIFI_PASSWORD
    #define CONFIG_MODEM_WIFI_PASSWORD      "12345678"
#endif
#define MODEM_WIFI_PASSWORD                 CONFIG_MODEM_WIFI_PASSWORD

#ifndef CONFIG_MODEM_WIFI_CHANNEL
    #define CONFIG_MODEM_WIFI_CHANNEL       6
#endif
#define MODEM_WIFI_CHANNEL                  CONFIG_MODEM_WIFI_CHANNEL

/* Wi-Fi Bandwidth Configuration */
#if defined(CONFIG_WIFI_BANDWIFTH_20)
    #define MODEM_WIFI_BANDWIDTH            WIFI_BW_HT20
#elif defined(CONFIG_WIFI_BANDWIFTH_40)
    #define MODEM_WIFI_BANDWIDTH            WIFI_BW_HT40
#else
    #define MODEM_WIFI_BANDWIDTH            WIFI_BW_HT40  // Default 40MHz
#endif

#ifndef CONFIG_MODEM_WIFI_MAX_STA
    #define CONFIG_MODEM_WIFI_MAX_STA       10
#endif
#define MODEM_WIFI_MAX_CONNECTIONS          CONFIG_MODEM_WIFI_MAX_STA

#ifndef CONFIG_MODEM_WIFI_DEFAULT_DNS
    #define CONFIG_MODEM_WIFI_DEFAULT_DNS   "8.8.8.8"
#endif
#define MODEM_WIFI_DNS_SERVER               CONFIG_MODEM_WIFI_DEFAULT_DNS

/* ==================== Utility Macros ==================== */

/**
 * @brief Check if modem supports dual CDC mode
 */
#define MODEM_HAS_DUAL_CDC()                MODEM_SUPPORT_SECONDARY_AT_PORT

/**
 * @brief Get modem name string
 */
#define MODEM_GET_NAME()                    MODEM_TARGET_NAME

/**
 * @brief Check if GPIO is enabled (non-zero)
 */
#define MODEM_GPIO_IS_ENABLED(gpio)         ((gpio) != 0)

#endif /* ESP_MODEM_USB_CONFIG_H */
