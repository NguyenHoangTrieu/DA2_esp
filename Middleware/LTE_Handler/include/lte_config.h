#ifndef _LTE_CONFIG_H_
#define _LTE_CONFIG_H_

/* Modem Device Selection */
#define LTE_CONFIG_MODEM_DEVICE_BG96    1
#define LTE_CONFIG_MODEM_DEVICE_SIM7600 2

#ifndef LTE_CONFIG_MODEM_DEVICE
#define LTE_CONFIG_MODEM_DEVICE         LTE_CONFIG_MODEM_DEVICE_BG96
#endif

/* PPP Authentication */
#ifndef LTE_CONFIG_PPP_AUTH_USERNAME
#define LTE_CONFIG_PPP_AUTH_USERNAME    "espressif"
#endif

#ifndef LTE_CONFIG_PPP_AUTH_PASSWORD
#define LTE_CONFIG_PPP_AUTH_PASSWORD    "esp32"
#endif

#ifndef LTE_CONFIG_PPP_AUTH_NONE
#define LTE_CONFIG_PPP_AUTH_NONE        0
#endif

/* SMS Configuration */
#ifndef LTE_CONFIG_SEND_MSG
#define LTE_CONFIG_SEND_MSG             0
#endif

#if LTE_CONFIG_SEND_MSG
#ifndef LTE_CONFIG_SEND_MSG_PEER_PHONE_NUMBER
#define LTE_CONFIG_SEND_MSG_PEER_PHONE_NUMBER   "+8610086"
#endif
#endif

#endif /* _LTE_CONFIG_H_ */
