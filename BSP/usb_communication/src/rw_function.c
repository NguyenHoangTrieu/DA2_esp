#include "usb_comm.h"

static const char *TAG = "USB_RW_FUNCTION";

void transfer_cb(usb_transfer_t *transfer) {
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
void cdc_async_receive_cb(usb_transfer_t *transfer) {
  usb_stream_t *stream = (usb_stream_t *)transfer->context;
  // Called from usb_host_client_handle_events() context ("task", not ISR)
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
      transfer->actual_num_bytes > 0) {
    // Allocate data object to push to FreeRTOS queue
    stream_data_t *rx = malloc(sizeof(stream_data_t));
    if (rx) {
      rx->data = malloc(transfer->actual_num_bytes);
      if (rx->data) {
        memcpy(rx->data, transfer->data_buffer, transfer->actual_num_bytes);
        rx->len = transfer->actual_num_bytes;
        // Non-blocking queue push. If queue full, free memory (avoid memory
        // leak)
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
void parse_and_cache_endpoints(usb_device_t *dev) {
  const usb_config_desc_t *config_desc = NULL;
  dev->ep_out_addr = 0x00;
  dev->ep_in_addr = 0x00;
  dev->interface_num = 0;

  ESP_ERROR_CHECK(
      usb_host_get_active_config_descriptor(dev->dev_hdl, &config_desc));

  uint8_t found_cdc_data_interface = 0;
  uint8_t target_interface = 0;

  // Phase 1: Scan all interfaces to find CDC Data class (0x0A) or suitable
  // interface
  int offset = 0;
  while (offset < config_desc->wTotalLength) {
    const usb_standard_desc_t *desc =
        (const usb_standard_desc_t *)(((const uint8_t *)config_desc) + offset);

    if (desc->bDescriptorType == USB_DESC_TYPE_INTERFACE) {
      const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;

      ESP_LOGI(TAG, "Interface found: num=%u, class=0x%02X, endpoints=%u",
               intf->bInterfaceNumber, intf->bInterfaceClass,
               intf->bNumEndpoints);

      // Prioritize CDC Data class (0x0A) for CDC ACM devices
      if (intf->bInterfaceClass == 0x0A && intf->bNumEndpoints >= 2) {
        target_interface = intf->bInterfaceNumber;
        found_cdc_data_interface = 1;
        ESP_LOGI(TAG, "Found CDC Data Interface %u (class 0x0A)",
                 target_interface);
        break; // Found CDC Data interface, use this one
      }

      // Fallback: Use first interface with >= 2 endpoints (vendor serial like
      // CH340)
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
        parsing_target_interface = 0; // Moved to different interface
      }
    } else if (desc->bDescriptorType == USB_DESC_TYPE_ENDPOINT &&
               parsing_target_interface) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
      uint8_t ep_addr = ep->bEndpointAddress;
      uint8_t ep_type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;

      // Only care about BULK endpoints for data transfer
      if (ep_type == USB_BM_ATTRIBUTES_XFER_BULK) {
        if (ep_addr & 0x80) {
          dev->ep_in_addr = ep_addr;
          ESP_LOGI(TAG, "-- Found BULK IN: addr=0x%02X, MPS=%u", ep_addr,
                   ep->wMaxPacketSize);
        } else {
          dev->ep_out_addr = ep_addr;
          ESP_LOGI(TAG, "-- Found BULK OUT: addr=0x%02X, MPS=%u", ep_addr,
                   ep->wMaxPacketSize);
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
    ESP_LOGE(TAG,
             "USB serial parsing failed. No valid BULK endpoints assigned!");
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
  if (!stream->data_queue)
    return ESP_ERR_NO_MEM;

  // Allocate buffer for incoming USB transfer
  esp_err_t ret =
      usb_host_transfer_alloc(USB_BUFFER_SIZE, 0, &stream->in_transfer);
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