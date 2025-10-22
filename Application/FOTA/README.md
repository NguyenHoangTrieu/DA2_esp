# Dual-MCU ESP32 Gateway OTA: WAN-as-Modem (PPP) Strategy

This document describes an efficient Over-The-Air (OTA) update strategy for an IoT Gateway that uses two ESP32-S3 microcontrollers (a **WAN MCU** and a **LAN MCU**).

In this architecture, only the **WAN MCU** has a direct internet connection (e.g., Wi-Fi). The **LAN MCU** is isolated. This strategy provides internet access to the LAN MCU by using the WAN MCU as a simple "modem" via a Point-to-Point Protocol (PPP) connection over UART.

This method is highly recommended as it relies entirely on standard, stable ESP-IDF components (`esp_httpss_ota`, `esp_netif_lwip_ppp`) and avoids complex, custom flasher protocols.

## Architecture Overview

The core idea is to create a standard IP network interface on the LAN MCU that tunnels all its traffic through the UART to the WAN MCU, which then routes it to the internet.

**Hardware:**

```
(Internet) <--> (Wi-Fi) <--> [ WAN MCU (ESP32S3) ] <--> (UART) <--> [ LAN MCU (ESP32S3) ]
```

**Software (During OTA):**

```
(OTA Server)
      |
(Wi-Fi Network)
      |
[ WAN MCU ]
  - esp_netif (Wi-Fi)
  - esp_netif (PPP Server)  <-- Provides IP to LAN MCU
  - IP NAPT (NAT) Forwarding  <-- Routes traffic
      |
(UART Connection)
      |
[ LAN MCU ]
  - esp_netif (PPP Client)  <-- Gets IP from WAN MCU
  - esp_httpss_ota          <-- Runs standard OTA, traffic uses PPP
```

## Key Features

  * **Uses Standard Libraries:** No custom bootloader, no re-implementing `esptool`. This entire flow uses the official `esp_httpss_ota` and `esp_netif_lwip_ppp` libraries.
  * **True Streaming Update:** The LAN MCU's binary file is streamed directly from the server *through* the WAN MCU to the LAN MCU's flash. The WAN MCU **never** needs to store the LAN MCU's binary file, saving flash space.
  * **Reliable & Robust:** This is a standard, well-tested networking stack (PPP + LWIP) provided by Espressif.
  * **Independent Logic:** Each MCU is responsible for updating itself. The logic remains clean and decoupled.

-----

## Implementation Details

You will need to configure each MCU separately.

### 1\. WAN MCU (PPP Server / "The Modem")

The WAN MCU's job is to connect to the internet (Wi-Fi) and run a PPP server on a UART port, forwarding any traffic from it to the Wi-Fi interface.

**Prerequisites:**

  * Enable `CONFIG_LWIP_IP_FORWARD` and `CONFIG_LWIP_NAPT` in `menuconfig` to allow routing.
  * Include `esp_netif_lwip_ppp.h` and `esp_netif.h`.

**Conceptual Steps:**

1.  **Initialize Wi-Fi:** Connect to your Wi-Fi AP as you normally would. Wait for the `IP_EVENT_GOT_IP` event.
2.  **Initialize PPP Server:**
      * Create a default PPP server network interface:
        ```c
        esp_netif_config_t ppp_netif_config = ESP_NETIF_DEFAULT_PPP_SERVER();
        esp_netif_t *ppp_netif = esp_netif_new(&ppp_netif_config);
        ```
      * Configure the UART for PPP. This involves setting the UART pins, baud rate, and attaching the PPP driver to the UART.
        ```c
        // Example: Configure UART for PPP
        uart_config_t uart_config = { /* ... set baud, pins, etc. ... */ };
        uart_driver_install(PPP_UART_PORT, BUF_SIZE, BUF_SIZE, 0, NULL, 0);

        // Attach the PPP driver to the UART
        esp_netif_ppp_config_t ppp_config = {
            .ppp_phase_event_cb = on_ppp_phase_changed, // Callback to know when client connects
            .ppp_error_event_cb = on_ppp_error,
        };
        esp_netif_attach_ppp_driver(ppp_netif, &ppp_config);
        uart_set_rx_full_threshold(PPP_UART_PORT, 64);
        esp_netif_ppp_start(ppp_netif); // Start the PPP server
        ```
3.  **Enable IP Forwarding (NAPT):**
      * Once the Wi-Fi interface (`wifi_netif`) is connected, enable NAPT (Network Address Port Translation) on it. This tells the WAN MCU to forward traffic from other interfaces (like our `ppp_netif`) out to the internet.
        ```c
        // After Wi-Fi has an IP
        esp_netif_napt_enable(wifi_netif);
        ESP_LOGI(TAG, "NAPT enabled on Wi-Fi interface");
        ```

### 2\. LAN MCU (PPP Client)

The LAN MCU's job is to connect to the WAN MCU's PPP server as a client. Once connected, it will have a standard network interface and can perform an OTA update as if it were connected directly to the internet.

**Prerequisites:**

  * Include `esp_netif_lwip_ppp.h` and `esp_httpss_ota.h`.
  * Ensure your `partitions.csv` file is configured for OTA (e.g., `ota_0`, `ota_1`, `otadata`).

**Conceptual Steps:**

1.  **Initialize PPP Client:**

      * Create a default PPP client network interface:
        ```c
        esp_netif_config_t ppp_netif_config = ESP_NETIF_DEFAULT_PPP_CLIENT();
        esp_netif_t *ppp_netif = esp_netif_new(&ppp_netif_config);
        ```
      * Configure the UART (must match the WAN MCU's settings) and attach the PPP driver.
        ```c
        // Configure UART (same settings as server)
        uart_config_t uart_config = { /* ... */ };
        uart_driver_install(PPP_UART_PORT, BUF_SIZE, BUF_SIZE, 0, NULL, 0);

        // Attach PPP driver
        esp_netif_ppp_config_t ppp_config = { /* ... set callbacks ... */ };
        esp_netif_attach_ppp_driver(ppp_netif, &ppp_config);
        uart_set_rx_full_threshold(PPP_UART_PORT, 64);
        esp_netif_ppp_start(ppp_netif); // Start the PPP client
        ```

2.  **Wait for Connection:**

      * Your event handler will receive `IP_EVENT_GOT_IP` once the PPP connection is established and the WAN MCU has assigned an IP address.
      * **At this point, the LAN MCU has full internet access.**

3.  **Run Standard OTA:**

      * Now that you have internet, simply call your normal `esp_httpss_ota` task. The HTTP client will automatically find and use the active `ppp_netif` interface as its gateway.
      * **No modifications to the standard OTA example code are needed.**
        ```c
        void start_ota_task(void *arg)
        {
            esp_http_client_config_t config = {
                .url = CONFIG_FIRMWARE_UPGRADE_URL_LAN, // URL for LAN MCU's binary
                .cert_pem = (char *)server_cert_pem_start,
                // ... other standard configs ...
            };

            esp_https_ota_config_t ota_config = {
                .http_config = &config,
            };

            esp_err_t ret = esp_https_ota(&ota_config);
            if (ret == ESP_OK) {
                // OTA Success. Notify WAN MCU before rebooting.
                uart_write_bytes(PPP_UART_PORT, "OTA_LAN_OK\n", 11);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                // OTA Failed. Notify WAN MCU.
                uart_write_bytes(PPP_UART_PORT, "OTA_LAN_FAIL\n", 13);
                vTaskDelete(NULL);
            }
        }

        // In your IP_EVENT_GOT_IP handler:
        xTaskCreate(&start_ota_task, "ota_task", 8192, NULL, 5, NULL);
        ```

-----

## Recommended Update Flow

To update the entire gateway safely:

1.  **Trigger:** An external command (e.g., MQTT) tells the **WAN MCU** to begin an update.
2.  **Update LAN First:**
      * WAN MCU sends a custom command (e.g., `START_OTA\n`) over the PPP-UART to the LAN MCU.
      * LAN MCU receives this, starts its PPP client, and runs its OTA process (as described above).
      * Upon success, LAN MCU sends a success message (e.g., `OTA_LAN_OK\n`) back to the WAN MCU and then reboots.
      * If it fails, it sends a failure message (e.g., `OTA_LAN_FAIL\n`).
3.  **Update WAN:**
      * The WAN MCU waits for the `OTA_LAN_OK` message.
      * Once received, the WAN MCU proceeds to run its *own* standard `esp_httpss_ota` task, downloading its firmware over Wi-Fi.
      * After its own successful update, the WAN MCU reboots.
4.  **Done:** The entire gateway is now updated. Updating the isolated LAN MCU first ensures that if the WAN MCU update fails, you don't lose the ability to retry the LAN update.