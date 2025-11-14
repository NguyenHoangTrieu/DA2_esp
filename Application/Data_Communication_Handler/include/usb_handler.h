#ifndef USB_HANDLER_H
#define USB_HANDLER_H

#include "usb_comm.h"
#include "config_handler.h"

// JTAG task control functions
void jtag_task_start(void);
void jtag_task_stop(void);

#endif // USB_HANDLER_H