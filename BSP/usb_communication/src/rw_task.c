#include "usb_handler.h"

static const char *TAG = "USB_OTG_RW";
static TaskHandle_t usb_otg_rw_task_hdl = NULL;
static bool close_usb_otg_rw_task = false;

#define USB_QUEUE_SIZE 10
#define USB_BUFFER_SIZE 64 // Adjust to endpoint MPS

// Structure to pass received data from ISR/callback to main USB RW task via queue
typedef struct {
    uint8_t *data;
    size_t len;
} stream_data_t;

// USB stream context object
typedef struct {
    usb_device_t *dev;            // Active device object
    usb_transfer_t *in_transfer;  // Pointer to transfer handle
    QueueHandle_t data_queue;     // FreeRTOS queue for data
    bool running;                 // Streaming flag
} usb_stream_t;


static void transfer_cb(usb_transfer_t *transfer) {
    usb_host_transfer_free(transfer);
}

/** Claims the interface for the given device.
 * This is necessary before performing any data transfers.
 * @param dev Pointer to usb_device_t struct with valid dev_hdl and
 * interface_num
 */
void claim_interface(usb_device_t *device_obj) {
  ESP_ERROR_CHECK(usb_host_interface_claim(device_obj->client_hdl,
                                           device_obj->dev_hdl,
                                           device_obj->interface_num, 0));
  ESP_LOGI(TAG, "Interface %d claimed for device addr %d",
           device_obj->interface_num, device_obj->dev_addr);
}

/** Callback for asynchronous IN transfers.
 * Called when data is received from the USB device.
 * Pushes received data to FreeRTOS queue for processing in main task context.
 * @param transfer Pointer to completed usb_transfer_t
 */
static void cdc_async_receive_cb(usb_transfer_t *transfer) {
    usb_stream_t *stream = (usb_stream_t *)transfer->context;
    // Called from usb_host_client_handle_events() context ("task", not ISR)
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        // Allocate data object to push to FreeRTOS queue
        stream_data_t *rx = malloc(sizeof(stream_data_t));
        if (rx) {
            rx->data = malloc(transfer->actual_num_bytes);
            if (rx->data) {
                memcpy(rx->data, transfer->data_buffer, transfer->actual_num_bytes);
                rx->len = transfer->actual_num_bytes;
                // Non-blocking queue push. If queue full, free memory (avoid memory leak)
                BaseType_t qret = xQueueSend(stream->data_queue, &rx, 0);
                if (qret != pdTRUE) {
                    free(rx->data);
                    free(rx);
                }
            } else {
                free(rx);
            }
        }
        // Resubmit transfer for continuous receiving if streaming is enabled
        if (stream->running) {
            usb_host_transfer_submit(transfer);
        }
    } else {
        // On device disconnect or error, stop streaming
        stream->running = false;
    }
}

/** Parses endpoints for CDC/Data class. Call in action_get_config_desc or after
 * enumeration. Caches endpoint addresses in usb_device_t struct for later use.
 * @param dev Pointer to usb_device_t struct with valid dev_hdl
 */
void parse_and_cache_endpoints(usb_device_t *dev) 
{
    const usb_config_desc_t *config_desc = NULL;
    dev->ep_out_addr = 0x00;
    dev->ep_in_addr = 0x00;
    dev->interface_num = 0;
    
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(dev->dev_hdl, &config_desc));
    
    uint8_t found_cdc_data_interface = 0;
    uint8_t target_interface = 0;
    
    // Phase 1: Scan all interfaces to find CDC Data class (0x0A) or suitable interface
    int offset = 0;
    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *desc = 
            (const usb_standard_desc_t *)(((const uint8_t *)config_desc) + offset);
        
        if (desc->bDescriptorType == USB_DESC_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            
            ESP_LOGI(TAG, "Interface found: num=%u, class=0x%02X, endpoints=%u",
                     intf->bInterfaceNumber, intf->bInterfaceClass, intf->bNumEndpoints);
            
            // Prioritize CDC Data class (0x0A) for CDC ACM devices
            if (intf->bInterfaceClass == 0x0A && intf->bNumEndpoints >= 2) {
                target_interface = intf->bInterfaceNumber;
                found_cdc_data_interface = 1;
                ESP_LOGI(TAG, "Found CDC Data Interface %u (class 0x0A)", target_interface);
                break;  // Found CDC Data interface, use this one
            }
            
            // Fallback: Use first interface with >= 2 endpoints (vendor serial like CH340)
            if (!found_cdc_data_interface && intf->bNumEndpoints >= 2) {
                target_interface = intf->bInterfaceNumber;
                ESP_LOGI(TAG, "Using vendor serial interface %u (class 0x%02X)", 
                         target_interface, intf->bInterfaceClass);
            }
        }
        
        offset += desc->bLength;
    }
    
    // Phase 2: Parse endpoints from the target interface
    offset = 0;
    uint8_t parsing_target_interface = 0;
    
    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *desc = 
            (const usb_standard_desc_t *)(((const uint8_t *)config_desc) + offset);
        
        if (desc->bDescriptorType == USB_DESC_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            
            if (intf->bInterfaceNumber == target_interface) {
                dev->interface_num = target_interface;
                parsing_target_interface = 1;
                ESP_LOGI(TAG, "Parsing endpoints for interface %u", target_interface);
            } else {
                parsing_target_interface = 0;  // Moved to different interface
            }
        }
        else if (desc->bDescriptorType == USB_DESC_TYPE_ENDPOINT && parsing_target_interface) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            uint8_t ep_addr = ep->bEndpointAddress;
            uint8_t ep_type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            
            // Only care about BULK endpoints for data transfer
            if (ep_type == USB_BM_ATTRIBUTES_XFER_BULK) {
                if (ep_addr & 0x80) {
                    dev->ep_in_addr = ep_addr;
                    ESP_LOGI(TAG, "-- Found BULK IN: addr=0x%02X, MPS=%u", 
                             ep_addr, ep->wMaxPacketSize);
                } else {
                    dev->ep_out_addr = ep_addr;
                    ESP_LOGI(TAG, "-- Found BULK OUT: addr=0x%02X, MPS=%u", 
                             ep_addr, ep->wMaxPacketSize);
                }
            }
            
            // If we have both endpoints, we're done
            if (dev->ep_in_addr != 0x00 && dev->ep_out_addr != 0x00) {
                break;
            }
        }
        
        offset += desc->bLength;
    }
    
    // Log final result
    ESP_LOGI(TAG, "Parsed endpoints: OUT=0x%02X, IN=0x%02X (Interface=%u)",
             dev->ep_out_addr, dev->ep_in_addr, dev->interface_num);
    
    if (dev->ep_out_addr == 0x00 || dev->ep_in_addr == 0x00) {
        ESP_LOGE(TAG, "USB serial parsing failed. No valid BULK endpoints assigned!");
    } else {
        ESP_LOGI(TAG, "Successfully parsed %s device", 
                 found_cdc_data_interface ? "CDC ACM" : "vendor serial");
    }
}
/**
 * @brief Send data to USB device via OUT endpoint
 *
 * @param[in] dev          USB device (usb_device_t)
 * @param[in] data         Data buffer to send
 * @param[in] len          Number of bytes in data buffer
 * @param[in] timeout_ms   Timeout in milliseconds
 * @return esp_err_t   ESP_OK if successful, error code otherwise
 */
esp_err_t usb_cdc_send_data(usb_device_t *dev, const uint8_t *data, size_t len,
                            int timeout_ms) {
  esp_err_t err;
  usb_transfer_t *transfer = NULL;

  if (!dev || dev->dev_hdl == NULL) {
    ESP_LOGE("USBOTG", "Invalid device handle");
    return ESP_ERR_INVALID_ARG;
  }

  if (dev->ep_out_addr == 0x00) {
    ESP_LOGE("USBOTG", "Invalid OUT endpoint address (0x00)");
    return ESP_ERR_INVALID_ARG;
  }

  err = usb_host_transfer_alloc(len, 0, &transfer);
  if (err != ESP_OK) {
    ESP_LOGE("USBOTG", "Failed to allocate transfer struct");
    return err;
  }

  transfer->device_handle = dev->dev_hdl;
  transfer->num_bytes = len;
  transfer->bEndpointAddress =
      dev->ep_out_addr; // endpoint_out needs to be initialized from descriptor
  transfer->timeout_ms = timeout_ms;
  transfer->callback = transfer_cb; // transfer_cb;
  memcpy(transfer->data_buffer, data, len);
  err = usb_host_transfer_submit(transfer);
  if (err == ESP_OK) {
    ESP_LOGI("USBOTG", "Sent %d bytes to device: endpoint 0x%02X", (int)len,
             dev->ep_out_addr);
  } else {
    ESP_LOGE("USBOTG", "USB Send failed: %d", err);
  }
  return err;
}


/**
 * Start USB CDC asynchronous data streaming.
 * @param dev   Pointer to USB device structure
 * @param stream Pointer to USB stream context
 * @return ESP_OK if success, otherwise error
 */
esp_err_t start_usb_cdc_streaming(usb_device_t *dev, usb_stream_t *stream) {
    // Associate the stream context with the USB device
    stream->dev = dev;

    // Create a queue for received data pointers
    stream->data_queue = xQueueCreate(USB_QUEUE_SIZE, sizeof(stream_data_t *));
    stream->running = true; // Mark stream as running

    // Check if queue was created successfully
    if (!stream->data_queue) return ESP_ERR_NO_MEM;

    // Allocate buffer for incoming USB transfer
    esp_err_t ret = usb_host_transfer_alloc(USB_BUFFER_SIZE, 0, &stream->in_transfer);
    if (ret != ESP_OK) {
        vQueueDelete(stream->data_queue); // Release resources if failed
        return ret;
    }

    // Set transfer device and endpoint info
    stream->in_transfer->device_handle = dev->dev_hdl;
    stream->in_transfer->bEndpointAddress = dev->ep_in_addr;
    stream->in_transfer->num_bytes = USB_BUFFER_SIZE;

    // Set callback and context for async reception
    stream->in_transfer->callback = cdc_async_receive_cb;
    stream->in_transfer->context = stream;

    // Submit transfer request for receiving data
    ret = usb_host_transfer_submit(stream->in_transfer);
    if (ret != ESP_OK) {
        usb_host_transfer_free(stream->in_transfer); // Clean up on failure
        vQueueDelete(stream->data_queue);
        return ret;
    }
    return ESP_OK; // Streaming started successfully
}

/** Set CH340 baudrate to 115200 via control transfers.
 * @param dev Pointer to usb_device_t struct with valid dev_hdl
 */
void ch340_set_baudrate(usb_device_t *dev) {
    // Calculate divisor for 115200 baud (based on fixed base clock)
    uint32_t divisor = 1532620800UL / 115200UL;
    if (divisor > 0) divisor--;
    uint16_t value = divisor & 0xFFFF; // Lower 16 bits for first setup
    uint16_t index = ((divisor >> 8) & 0xFF) | 0x0080; // Part for second setup

    // ---- First control transfer to set base baudrate value ----
    usb_transfer_t *ctrl1 = NULL;
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl1));
    ctrl1->device_handle = dev->dev_hdl;
    ctrl1->bEndpointAddress = 0; // Endpoint 0 is default control endpoint
    ctrl1->callback = transfer_cb;
    ctrl1->context = NULL;
    ctrl1->num_bytes = sizeof(usb_setup_packet_t);

    // Construct setup packet for first control transfer
    usb_setup_packet_t setup1 = {
        .bmRequestType = 0x40,        // Vendor, Host to Device
        .bRequest      = 0x9A,        // CH340 vendor request
        .wValue        = 0x1312,      // Specific for setting baudrate
        .wIndex        = value,       // Lower part of divisor
        .wLength       = 0,
    };
    memcpy(ctrl1->data_buffer, &setup1, sizeof(setup1));
    ESP_ERROR_CHECK(usb_host_transfer_submit_control(dev->client_hdl, ctrl1));
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for transaction to complete

    // ---- Second control transfer to set upper baudrate value ----
    usb_transfer_t *ctrl2 = NULL;
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl2));
    ctrl2->device_handle = dev->dev_hdl;
    ctrl2->bEndpointAddress = 0;
    ctrl2->callback = transfer_cb;
    ctrl2->context = NULL;
    ctrl2->num_bytes = sizeof(usb_setup_packet_t);

    // Construct setup packet for second control transfer
    usb_setup_packet_t setup2 = {
        .bmRequestType = 0x40,
        .bRequest      = 0x9A,
        .wValue        = 0x0F2C,    // Second part for baudrate process
        .wIndex        = index,     // Upper part of divisor (with flag)
        .wLength       = 0,
    };
    memcpy(ctrl2->data_buffer, &setup2, sizeof(setup2));
    ESP_ERROR_CHECK(usb_host_transfer_submit_control(dev->client_hdl, ctrl2));
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for transaction

    // Log baudrate configuration for debugging
    ESP_LOGI("CH340", "Configured baudrate to 115200 for CH340.");
}

// =============================================================================

/**
 * @brief Find first available USB device
 * @return Pointer to usb_device_t or NULL if not found
 */
static usb_device_t* find_first_usb_device(void) {
    usb_device_t *dev = NULL;
    
    // Lock mutex to safely access device list
    xSemaphoreTake(s_driver_obj->constant.mux_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
        if (s_driver_obj->mux_protected.device[i].dev_hdl != NULL) {
            dev = &s_driver_obj->mux_protected.device[i];
            break;
        }
    }
    xSemaphoreGive(s_driver_obj->constant.mux_lock);
    
    return dev;
}

/**
 * @brief Check if device is valid serial device
 * @param dev USB device pointer
 * @return true if valid, false otherwise
 */
static bool is_valid_serial_device(usb_device_t *dev) {
    if (dev->ep_out_addr == 0x00 || dev->ep_in_addr == 0x00) {
        ESP_LOGW(TAG, "Device addr %d is not valid serial. Skipping.", dev->dev_addr);
        return false;
    }
    return true;
}

/**
 * @brief Configure USB device (baudrate + streaming)
 * @param dev USB device pointer
 * @param stream USB stream context pointer
 * @return ESP_OK on success
 */
static esp_err_t configure_usb_device_once(usb_device_t *dev, usb_stream_t *stream) {
    ch340_set_baudrate(dev);
    ESP_LOGI(TAG, "CH340 baudrate set");
    
    esp_err_t ret = start_usb_cdc_streaming(dev, stream);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start streaming: %d", ret);
        return ret;
    }
    
    return ESP_OK;
}

/**
 * @brief Check if device still connected
 * @param dev USB device pointer
 * @return ESP_OK if valid, error code otherwise
 */
static esp_err_t check_device_validity(usb_device_t *dev) {
    usb_device_info_t dev_info;
    esp_err_t err = usb_host_device_info(dev->dev_hdl, &dev_info);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Device invalid (err: %d)", err);
    }
    
    return err;
}

/**
 * @brief Send data via USB CDC
 * @param dev USB device pointer
 * @param data Data buffer
 * @param len Data length
 * @return ESP_OK on success
 */
static esp_err_t send_usb_data(usb_device_t *dev, const uint8_t *data, size_t len) {
    led_show_blue();
    
    esp_err_t ret = usb_cdc_send_data(dev, data, len, 100);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sent %d bytes", (int)len);
    } else {
        ESP_LOGE(TAG, "Send error: %d", ret);
        led_show_red();
    }
    
    return ret;
}

/**
 * @brief Receive and process USB data (non-blocking)
 * @param stream USB stream context pointer
 */
static void receive_and_process_usb_data(usb_stream_t *stream) {
    stream_data_t *rx = NULL;
    
    // Poll queue for received data
    while (xQueueReceive(stream->data_queue, &rx, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Received %d bytes:", (int)rx->len);
        
        for (size_t i = 0; i < rx->len; ++i) {
            printf("%c", rx->data[i]);
        }
        
        free(rx->data);
        free(rx);
    }
}

/**
 * @brief Handle connected USB device
 * @param dev USB device pointer
 * @param stream USB stream context
 * @param configured Configuration flag pointer
 * @param tx_data Transmit data buffer
 * @param tx_len Transmit data length
 * @return ESP_OK on success
 */
static esp_err_t handle_connected_device(usb_device_t *dev, 
                                         usb_stream_t *stream,
                                         uint8_t *configured,
                                         const uint8_t *tx_data,
                                         size_t tx_len) {
    // Validate serial device
    if (!is_valid_serial_device(dev)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        return ESP_FAIL;
    }
    
    // Configure device on first detection
    if (*configured == 0) {
        esp_err_t ret = configure_usb_device_once(dev, stream);
        if (ret != ESP_OK) return ret;
        *configured = 1;
    }
    
    // Check device still valid
    esp_err_t err = check_device_validity(dev);
    if (err != ESP_OK) {
        stream->running = false;
        *configured = 0;
        led_show_red();
        vTaskDelay(pdMS_TO_TICKS(1000));
        return err;
    }
    
    // Send data
    send_usb_data(dev, tx_data, tx_len);
    
    // Receive and process data
    receive_and_process_usb_data(stream);
    
    led_show_green();
    return ESP_OK;
}

/**
 * @brief Handle no device state
 * @param stream USB stream context
 * @param configured Configuration flag pointer
 */
static void handle_no_device(usb_stream_t *stream, uint8_t *configured) {
    if (stream->running) {
        stream->running = false;
    }
    
    *configured = 0;
    ESP_LOGW(TAG, "No USB device. Task idle.");
    led_toggle_white();
}

/**
 * @brief USB OTG read/write task
 * @param arg Task parameter (unused)
 */
void usb_otg_rw_task(void *arg) {
    uint8_t tx_data[] = {'N', 'A', 'T', 'E', ' ', 'H', 'I', 'G', 'G', 'E', 'R', '\n'};
    static usb_stream_t usb_stream;
    static uint8_t configured = 0;
    
    while (!close_usb_otg_rw_task) {
        // Find first USB device
        usb_device_t *dev = find_first_usb_device();
        
        if (dev != NULL) {
            // Handle connected device
            handle_connected_device(dev, &usb_stream, &configured, 
                                   tx_data, sizeof(tx_data));
        } else {
            // Handle no device state
            handle_no_device(&usb_stream, &configured);
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI(TAG, "USB OTG RW task exiting");
    vTaskDelete(NULL);
}


// =============================================================================

void usb_otg_rw_task_start(void) {
    close_usb_otg_rw_task = false;
    BaseType_t task_created = xTaskCreatePinnedToCore(usb_otg_rw_task,
                                           "usb_otg_rw",
                                           4 * 1024,
                                           NULL,
                                           RW_TASK_PRIORITY,
                                           &usb_otg_rw_task_hdl,
                                           0);
    assert(task_created == pdTRUE);
}

void usb_otg_rw_task_stop(void) {
    close_usb_otg_rw_task = true;
}