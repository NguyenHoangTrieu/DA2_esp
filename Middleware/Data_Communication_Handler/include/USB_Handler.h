#ifndef USB_HANDLER_H
#define USB_HANDLER_H

#include "usb_comm.h"

void usb_host_lib_task(void *arg);
void class_driver_task(void *arg);
void usb_otg_rw_task(void *arg);
void parse_and_cache_endpoints(usb_device_t *dev);
void claim_interface(usb_device_t *device_obj);
// Class driver task control functions
void class_driver_task_start(void);
void class_driver_task_stop(void);
// USB host library task control functions
void usb_host_lib_task_start(void);
void usb_host_lib_task_stop(void);
// RW task control functions
void usb_otg_rw_task_start(void);
void usb_otg_rw_task_stop(void);
// JTAG task control functions
void jtag_task_start(void);
void jtag_task_stop(void);

#endif // USB_HANDLER_H