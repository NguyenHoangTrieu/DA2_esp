#ifndef ESP_MODEM_USB_DTE_INTERNAL_H_
#define ESP_MODEM_USB_DTE_INTERNAL_H_

#include "esp_log.h"
#include "esp_modem_usb.h"

/**
* @brief Macro defined for error checking
*
*/
#define ESP_MODEM_USB_ERR_CHECK(a, str, goto_tag, ...)                                    \
    do                                                                                \
    {                                                                                 \
        if (!(a))                                                                     \
        {                                                                             \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                            \
        }                                                                             \
    } while (0)

/**
* @brief common modem delay function
*
*/
static inline void esp_modem_usb_wait_ms(size_t time)
{
    vTaskDelay(pdMS_TO_TICKS(time));
}

#endif