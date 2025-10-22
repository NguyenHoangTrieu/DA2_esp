/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "USB_Handler.h"

// =============================================================================
// USB SERIAL JTAG Echo Task
// =============================================================================

#define BUF_SIZE (1024)

const char* TAG = "USB_HANDLER";

static TaskHandle_t jtag_task_hdl = NULL;
static TaskHandle_t usb_host_task_hdl = NULL;
static TaskHandle_t class_driver_task_hdl = NULL;
static TaskHandle_t usb_otg_rw_task_hdl = NULL;
static usb_serial_jtag_driver_config_t usb_serial_jtag_config;
static bool close_jtag_task = false;
static bool close_usb_otg_rw_task = false;
static bool usb_host_lib_close = false;

static void jtag_task(void *arg)
{           
    // Configure USB SERIAL JTAG
    usb_serial_jtag_config.rx_buffer_size = BUF_SIZE;
    usb_serial_jtag_config.tx_buffer_size = BUF_SIZE;

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
    ESP_LOGI("usb_serial_jtag echo", "USB_SERIAL_JTAG init done");
    
    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE("usb_serial_jtag echo", "no memory for data");
        return;
    }

    while (!close_jtag_task) {

        int len = usb_serial_jtag_read_bytes(data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);

        // Write data back to the USB SERIAL JTAG
        if (len) {
            usb_serial_jtag_write_bytes((const char *) data, len, 20 / portTICK_PERIOD_MS);
            data[len] = '\0';
            ESP_LOG_BUFFER_HEXDUMP("Recv str: ", data, len, ESP_LOG_INFO);
        }
    }
    usb_serial_jtag_driver_uninstall();
    ESP_LOGI("usb_serial_jtag echo", "USB_SERIAL_JTAG deinit done");
    vTaskDelete(NULL);
}

// =============================================================================

void jtag_task_start(void)
{
    close_jtag_task = false;
    BaseType_t task_created;
    // Create jtag task
    task_created = xTaskCreatePinnedToCore(jtag_task,
                                           "usb_serial_jtag_echo",
                                           4096,
                                           NULL,
                                           JTAG_TASK_PRIORITY,
                                           &jtag_task_hdl,
                                           0);
    assert(task_created == pdTRUE);
}

void jtag_task_stop(void)
{
    close_jtag_task = true;
}

// =============================================================================
// USB Read Write Task
// =============================================================================

/** Set CH340 baudrate to 115200 via control transfers.
 * @param dev Pointer to usb_device_t struct with valid dev_hdl
 */
void ch340_set_baudrate(usb_device_t *dev) {
  // Calculate divisor for 115200 baud (based on fixed base clock)
  uint32_t divisor = 1532620800UL / 115200UL;
  if (divisor > 0)
    divisor--;
  uint16_t value = divisor & 0xFFFF; // Lower 16 bits for first setup
  uint16_t index = ((divisor >> 8) & 0xFF) | 0x0080; // Part for second setup

  // ---- First control transfer to set base baudrate value ----
  usb_transfer_t *ctrl1 = NULL;
  ESP_ERROR_CHECK(
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl1));
  ctrl1->device_handle = dev->dev_hdl;
  ctrl1->bEndpointAddress = 0; // Endpoint 0 is default control endpoint
  ctrl1->callback = transfer_cb;
  ctrl1->context = NULL;
  ctrl1->num_bytes = sizeof(usb_setup_packet_t);

  // Construct setup packet for first control transfer
  usb_setup_packet_t setup1 = {
      .bmRequestType = 0x40, // Vendor, Host to Device
      .bRequest = 0x9A,      // CH340 vendor request
      .wValue = 0x1312,      // Specific for setting baudrate
      .wIndex = value,       // Lower part of divisor
      .wLength = 0,
  };
  memcpy(ctrl1->data_buffer, &setup1, sizeof(setup1));
  ESP_ERROR_CHECK(usb_host_transfer_submit_control(dev->client_hdl, ctrl1));
  vTaskDelay(pdMS_TO_TICKS(10)); // Wait for transaction to complete

  // ---- Second control transfer to set upper baudrate value ----
  usb_transfer_t *ctrl2 = NULL;
  ESP_ERROR_CHECK(
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl2));
  ctrl2->device_handle = dev->dev_hdl;
  ctrl2->bEndpointAddress = 0;
  ctrl2->callback = transfer_cb;
  ctrl2->context = NULL;
  ctrl2->num_bytes = sizeof(usb_setup_packet_t);

  // Construct setup packet for second control transfer
  usb_setup_packet_t setup2 = {
      .bmRequestType = 0x40,
      .bRequest = 0x9A,
      .wValue = 0x0F2C, // Second part for baudrate process
      .wIndex = index,  // Upper part of divisor (with flag)
      .wLength = 0,
  };
  memcpy(ctrl2->data_buffer, &setup2, sizeof(setup2));
  ESP_ERROR_CHECK(usb_host_transfer_submit_control(dev->client_hdl, ctrl2));
  vTaskDelay(pdMS_TO_TICKS(10)); // Wait for transaction

  // Log baudrate configuration for debugging
  ESP_LOGI("CH340", "Configured baudrate to 115200 for CH340.");
}

// =============================================================================

// USB OTG Read/Write Task
void usb_otg_rw_task(void *arg) {
    uint8_t tx_data[] = {'N', 'A', 'T', 'E', ' ', 'H', 'I', 'G', 'G', 'E', 'R', '\n'};
    static usb_stream_t usb_stream; // Persistent stream object; works across reconnect
    static uint8_t configured = 0;

    while (!close_usb_otg_rw_task) {
        usb_device_t *dev = NULL;

        // Critical section to select the first opened USB device
        xSemaphoreTake(s_driver_obj->constant.mux_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
            if (s_driver_obj->mux_protected.device[i].dev_hdl != NULL) {
                dev = &s_driver_obj->mux_protected.device[i];
                break;
            }
        }
        xSemaphoreGive(s_driver_obj->constant.mux_lock);

        if (dev != NULL) {
            if (dev->ep_out_addr == 0x00 || dev->ep_in_addr == 0x00) {
                ESP_LOGW("USB_OTG_RW", "Device addr %d is not a valid serial device. Skipping.", dev->dev_addr);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            // One-time configuration for a newly detected device
            if (configured == 0) {
                ch340_set_baudrate(dev);
                configured = 1;
                ESP_LOGI("USB_OTG_RW", "CH340 baudrate set");
                // Start streaming for this device (setup rx queue)
                start_usb_cdc_streaming(dev, &usb_stream);
            }
            // Check if device is still attached/valid
            usb_device_info_t dev_info;
            esp_err_t err = usb_host_device_info(dev->dev_hdl, &dev_info);
            if (err != ESP_OK) {
                ESP_LOGE("USB_OTG_RW", "Device invalid (err: %d). Stopping.", err);
                usb_stream.running = false;
                configured = 0;
                led_show_red();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue; // Device now invalid
            }
            // Send data over CDC
            led_show_blue();
            esp_err_t send_ret = usb_cdc_send_data(dev, tx_data, sizeof(tx_data), 100);
            if (send_ret == ESP_OK) {
                ESP_LOGI("USB_OTG_RW", "Sent %d bytes.", (int)sizeof(tx_data));
            } else {
                ESP_LOGE("USB_OTG_RW", "Send error: %d", send_ret);
                led_show_red();
            }
            // Non-blocking receive: poll queue for new data, print any available
            stream_data_t *rx = NULL;
            while (xQueueReceive(usb_stream.data_queue, &rx, 0) == pdTRUE) {
                ESP_LOGI("USB_OTG_RW", "Received %d bytes:", (int)rx->len);
                for (size_t i = 0; i < rx->len; ++i) {
                    printf("%c", rx->data[i]);
                }
                free(rx->data); free(rx);
            }
            led_show_green();
        } else {
            // No device present: clean up state and indicate idle
            if (usb_stream.running) {
                usb_stream.running = false;
            }
            configured = 0;
            ESP_LOGW("USB_OTG_RW", "No USB device opened. Task idle.");
            led_toggle_white();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI("USB_OTG_RW", "USB OTG RW task exiting.");
    vTaskDelete(NULL);
}

// =============================================================================

void usb_otg_rw_task_start(void) {
  close_usb_otg_rw_task = false;
  BaseType_t task_created =
      xTaskCreatePinnedToCore(usb_otg_rw_task, "usb_otg_rw", 4 * 1024, NULL,
                              RW_TASK_PRIORITY, &usb_otg_rw_task_hdl, 0);
  assert(task_created == pdTRUE);
}

void usb_otg_rw_task_stop(void) { close_usb_otg_rw_task = true; }

// =============================================================================
/**
 * @brief Start USB Host install and handle common USB host library events while
 * app pin not low
 *
 * @param[in] arg  Not used
 */
void usb_host_lib_task(void *arg) {
  bool has_clients = true;
  bool has_devices = false;
  while (has_clients && !usb_host_lib_close) {
    uint32_t event_flags;
    ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
      if (ESP_OK == usb_host_device_free_all()) {
        ESP_LOGI(
            TAG,
            "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
        has_clients = false;
      } else {
        ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
        has_devices = true;
      }
    }
    if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
      has_clients = false;
    }
  }
  ESP_LOGI(TAG, "No more clients and devices or close task, uninstall USB Host library");
  // Uninstall the USB Host Library
  ESP_ERROR_CHECK(usb_host_uninstall());
  vTaskDelete(NULL);
}

// =============================================================================

void usb_host_lib_task_start(void){
  usb_host_lib_init();
  ESP_LOGI(TAG, "USB Host init done");
  usb_host_lib_close = false;
  BaseType_t task_created;
  // Create usb host lib task
  task_created = xTaskCreatePinnedToCore(usb_host_lib_task,
                                         "usb_host",
                                         4096,
                                         NULL,
                                         HOST_LIB_TASK_PRIORITY,
                                         &usb_host_task_hdl,
                                         0);
  assert(task_created == pdTRUE);
}

void usb_host_lib_task_stop(void){
  usb_host_lib_close = true;
  ESP_LOGI(TAG, "USB Host deinit done");
}

// =============================================================================
/** Class driver main task.
 * Handles USB client events and device actions.
 * @param arg Not used
 */
void class_driver_task(void *arg) {
  while (1) {
    // Driver has unhandled devices, handle all devices first
    ESP_LOGI(TAG, "Class Driver Active");
    if (m_driver_obj.mux_protected.flags.unhandled_devices) {
      xSemaphoreTake(m_driver_obj.constant.mux_lock,
                     portMAX_DELAY); // Acquire mutex for thread safety
      for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
        if (m_driver_obj.mux_protected.device[i].actions) {
          class_driver_device_handle(
              &m_driver_obj.mux_protected.device[i]); // Process device actions
        }
      }
      m_driver_obj.mux_protected.flags.unhandled_devices = 0;
      xSemaphoreGive(m_driver_obj.constant.mux_lock); // Release mutex
    } else {
      // Driver is active, handle client events
      if (m_driver_obj.mux_protected.flags.shutdown == 0) {
        usb_host_client_handle_events(
            class_driver_client_hdl,
            portMAX_DELAY); // Wait for and handle USB client events
      } else {
        // Shutdown the driver
        break;
      }
    }
  }

  ESP_LOGI(TAG, "Deregistering Class Client");
  ESP_ERROR_CHECK(usb_host_client_deregister(
      class_driver_client_hdl)); // Deregister client from USB Host Library
  vTaskDelete(NULL); // Delete task after cleanup
}

void class_driver_task_start(void){
  class_driver_init();
  ESP_LOGI(TAG, "Class Driver init done");
  BaseType_t task_created;
  // Create class driver task
  task_created = xTaskCreatePinnedToCore(class_driver_task,
                                        "class",
                                        4 * 1024,
                                        NULL,
                                        CLASS_TASK_PRIORITY,
                                        &class_driver_task_hdl,
                                        0);
  assert(task_created == pdTRUE);
}

void class_driver_task_stop(void){
  class_driver_deinit();
  ESP_LOGI(TAG, "Class Driver deinit done");
}