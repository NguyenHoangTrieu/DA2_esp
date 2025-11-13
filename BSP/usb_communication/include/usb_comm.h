#ifndef USB_COMM_H
#define USB_COMM_H

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "mbedtls/md5.h"
#include "rbg_handler.h"
#include "sdkconfig.h"
#include "esp_check.h"

#define CLIENT_NUM_EVENT_MSG        5
#define CONFIG_APP_QUIT_PIN       5

#define HOST_LIB_TASK_PRIORITY  2
#define CLASS_TASK_PRIORITY     3
#define RW_TASK_PRIORITY        3
#define JTAG_TASK_PRIORITY      3
#define APP_QUIT_PIN            CONFIG_APP_QUIT_PIN

typedef enum {
    ACTION_OPEN_DEV         = (1 << 0),
    ACTION_GET_DEV_INFO     = (1 << 1),
    ACTION_GET_DEV_DESC     = (1 << 2),
    ACTION_GET_CONFIG_DESC  = (1 << 3),
    ACTION_GET_STR_DESC     = (1 << 4),
    ACTION_CLOSE_DEV        = (1 << 5),
} action_t;

#define USB_DESC_TYPE_INTERFACE 0x04
#define USB_DESC_TYPE_ENDPOINT  0x05
#define CDC_DATA_INTERFACE_CLASS 0x0A // CDC Data class

#define DEV_MAX_COUNT           128

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    action_t actions;

    // --- Add for endpoint/interface tracking ---
    uint8_t interface_num;      // CDC interface number found.
    uint8_t ep_out_addr;        // OUT endpoint address for data (parse from descriptor)
    uint8_t ep_in_addr;         // IN endpoint address for data (parse from descriptor)
} usb_device_t;

typedef struct {
    struct {
        union {
            struct {
                uint8_t unhandled_devices: 1;   /**< Device has unhandled devices */
                uint8_t shutdown: 1;            /**<  */
                uint8_t reserved6: 6;           /**< Reserved */
            };
            uint8_t val;                        /**< Class drivers' flags value */
        } flags;                                /**< Class drivers' flags */
        usb_device_t device[DEV_MAX_COUNT];     /**< Class drivers' static array of devices */
    } mux_protected;                            /**< Mutex protected members. Must be protected by the Class mux_lock when accessed */

    struct {
        usb_host_client_handle_t client_hdl;
        SemaphoreHandle_t mux_lock;         /**< Mutex for protected members */
    } constant;                                 /**< Constant members. Do not change after installation thus do not require a critical section or mutex */
} class_driver_t;

extern class_driver_t *s_driver_obj;
#define USB_QUEUE_SIZE 10
#define USB_BUFFER_SIZE 64 // Adjust to endpoint MPS

// Structure to pass received data from ISR/callback to main USB RW task via
// queue
typedef struct {
  uint8_t *data;
  size_t len;
} stream_data_t;

// USB stream context object
typedef struct {
  usb_device_t *dev;           // Active device object
  usb_transfer_t *in_transfer; // Pointer to transfer handle
  QueueHandle_t data_queue;    // FreeRTOS queue for data
  bool running;                // Streaming flag
} usb_stream_t;

extern class_driver_t *s_driver_obj;
extern class_driver_t m_driver_obj;
extern usb_host_client_handle_t class_driver_client_hdl;

extern void class_driver_client_deregister(void);

// USB Host class driver functions
void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);
void action_open_dev(usb_device_t *device_obj);
void action_get_info(usb_device_t *device_obj);
void action_get_dev_desc(usb_device_t *device_obj);
void action_get_config_desc(usb_device_t *device_obj);
void action_get_str_desc(usb_device_t *device_obj);
void action_close_dev(usb_device_t *device_obj);
void class_driver_device_handle(usb_device_t *device_obj);
void class_driver_client_deregister(void);
void class_driver_init(void);
void class_driver_deinit(void);
void usb_host_lib_init(void);
void usb_host_lib_deinit(void);

#endif // USB_COMM_H