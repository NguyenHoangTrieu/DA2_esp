# Web Config Portal — DA2 ESP32-S3 WAN Gateway
## Implementation Plan

---

## 1. Partition Table Decision

Current `partitions.csv`:
```
nvs,      data, nvs,     0x9000,   0x6000,   →  24 KB   NVS key-value store
otadata,  data, ota,     0xF000,   0x2000,   →   8 KB   OTA slot tracker
phy_init, data, phy,     0x11000,  0x1000,   →   4 KB   RF calibration
ota_0,    app,  ota_0,   0x20000,  7M,       →   7 MB   Active firmware slot
ota_1,    app,  ota_1,   ,         7M,       →   7 MB   OTA update target slot
```

Flash map:  
- ota_1 ends at `0xE20000`  
- 16 MB flash ends at `0x1000000`  
- **Unallocated tail: `0x1E0000` ≈ 1.875 MB**

### Decision: `EMBED_TXTFILES` — **do NOT add a filesystem partition**

| Criteria | EMBED_TXTFILES ✅ | LittleFS partition ❌ |
|---|---|---|
| Partition table change needed | No | Yes — shrink OTA or use 1.875 MB tail |
| Web UI always in sync with firmware | Yes — same binary | No — version mismatch risk |
| Update web UI independently | No (needs reflash) | Yes (but you don't need this) |
| Complexity in C code | Minimal (extern pointer) | Mount FS, handle errors |
| Breaks existing FOTA flow | Never | Possible if OTA shrunk |
| Typical SPA output size | 150–350 KB (fits easily in 7 MB OTA) | Same |

**Key reasoning:** The web UI is tightly coupled to this firmware's exact C structs (`wifi_config_data_t`, `lte_config_data_t`, `mqtt_config_data_t`, etc.). They must be updated together. EMBED_TXTFILES enforces this coupling automatically. A LittleFS partition only adds value when the UI team updates pages independently — which does not apply here.

**No partitions.csv change required.**

---

## 2. Architecture Overview

```
Browser
  │
  ├─ GET  /              → ESP32 serves embedded index.html (Vite SPA, one file)
  ├─ GET  /api/config    → ESP32 reads NVS globals → JSON response
  ├─ POST /api/config    → ESP32 parses JSON → pushes to g_config_handler_queue
  ├─ GET  /api/status    → ESP32 returns live connectivity state
  └─ POST /api/reboot    → esp_restart()

AP mode (first boot / no credentials):
  ├─ ESP32 starts WiFi AP  "DA2-Gateway-XXXX"
  ├─ DNS server answers ALL queries with 192.168.4.1  (captive portal)
  └─ Browser auto-opens portal page

STA mode (after credentials saved):
  ├─ ESP32 connects to user's WiFi
  ├─ mDNS registers "gateway.local"
  └─ Web server stays running on STA IP
```

The web server integrates as a **new command source** alongside UART and USB. It uses the exact same `g_config_handler_queue` that `uart_handler.c` already uses — zero changes to config_handler.c logic.

---

## 3. New Component: `Application/Web_Config_Handler/`

```
Application/
└── Web_Config_Handler/
    ├── include/
    │   └── web_config_handler.h    ← public API
    └── src/
        ├── web_server.c            ← httpd lifecycle, route registration, index.html serving
        ├── api_config.c            ← GET /api/config, POST /api/config
        ├── api_status.c            ← GET /api/status, POST /api/reboot
        └── captive_dns.c           ← UDP DNS server for AP captive portal
```

**Web front-end (separate build step, output goes into firmware):**
```
Application/Web_Config_Handler/
└── web/
    ├── index.html          ← final build output (embedded into firmware via CMake)
    └── src/                ← source files (not compiled by IDF, managed by Vite)
        ├── main.js
        ├── tabs/
        │   ├── wifi.js
        │   ├── lte.js
        │   ├── server.js
        │   └── status.js
        └── style.css
```

---

## 4. API Contract

### `GET /api/config`
Returns current configuration read from NVS globals (already loaded at boot by `config_init()`).

**Response:**
```json
{
  "internet_type": 0,
  "server_type": 0,
  "wifi": {
    "ssid": "MyNetwork",
    "password": "",
    "username": "",
    "auth_mode": 0
  },
  "lte": {
    "apn": "internet",
    "username": "",
    "password": "",
    "auto_reconnect": true
  },
  "mqtt": {
    "broker_uri": "mqtt://demo.thingsboard.io",
    "device_token": "...",
    "subscribe_topic": "...",
    "publish_topic": "..."
  },
  "http": {
    "server_url": "http://...",
    "auth_token": "...",
    "use_tls": false,
    "verify_server": false,
    "timeout_ms": 10000
  },
  "coap": {
    "host": "demo.thingsboard.io",
    "resource_path": "/api/v1/{token}/telemetry",
    "device_token": "...",
    "port": 5683,
    "use_dtls": false
  }
}
```

### `POST /api/config`
Accepts partial or full config JSON. Each present section triggers a corresponding command pushed to `g_config_handler_queue` — identical to how `uart_handler.c` sends commands today.

**Body (example — update WiFi only):**
```json
{
  "wifi": {
    "ssid": "NewNetwork",
    "password": "secret"
  }
}
```

**Response:**
```json
{ "ok": true }
```
or
```json
{ "ok": false, "error": "Invalid JSON" }
```

### `GET /api/status`
```json
{
  "firmware_version": "1.0.1",
  "internet_type": "wifi",
  "wifi_connected": true,
  "wifi_rssi": -62,
  "lte_connected": false,
  "server_type": "mqtt",
  "mqtt_connected": true,
  "uptime_s": 3842
}
```

### `POST /api/reboot`
```json
{ "ok": true }
```
Calls `esp_restart()` after 500ms delay.

---

## 5. C Implementation Details

### 5.1 `web_config_handler.h`
```c
#ifndef WEB_CONFIG_HANDLER_H
#define WEB_CONFIG_HANDLER_H

#include "esp_err.h"

typedef enum {
    WEB_MODE_AP,   // Running in AP mode — captive portal active
    WEB_MODE_STA,  // Running in STA mode — normal web server
} web_server_mode_t;

void web_config_handler_start(web_server_mode_t mode);
void web_config_handler_stop(void);
web_server_mode_t web_config_handler_get_mode(void);

#endif
```

### 5.2 `web_server.c` — Key sections

**Embedded file access (generated by CMake EMBED_TXTFILES):**
```c
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    httpd_resp_send(req, index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}
```

**Route registration:**
```c
static const httpd_uri_t routes[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = root_get_handler   },
    { .uri = "/api/config",  .method = HTTP_GET,  .handler = api_config_get     },
    { .uri = "/api/config",  .method = HTTP_POST, .handler = api_config_post    },
    { .uri = "/api/status",  .method = HTTP_GET,  .handler = api_status_get     },
    { .uri = "/api/reboot",  .method = HTTP_POST, .handler = api_reboot_post    },
    // Captive portal catch-all — redirect any other URL to /
    { .uri = "/*",           .method = HTTP_GET,  .handler = redirect_handler   },
};
```

### 5.3 `api_config.c` — POST handler integration

The POST handler builds a `config_command_t` and sends it to the **existing queue** — no changes to config_handler.c needed:

```c
static esp_err_t api_config_post(httpd_req_t *req) {
    char body[CONFIG_CMD_MAX_LEN];
    int  received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    // Parse which sections are present and enqueue corresponding commands
    // e.g. if "wifi" key found: build "WF:..." command string and enqueue
    // Reuses the same config_parse_* helpers from config_handler.c

    config_command_t cmd = {
        .type    = CONFIG_TYPE_WIFI,   // set per section found
        .source  = CMD_SOURCE_HTTP,    // new source enum value
        .data_len = (uint16_t)received,
    };
    memcpy(cmd.raw_data, body, received);
    xQueueSend(g_config_handler_queue, &cmd, pdMS_TO_TICKS(500));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

> **Note:** Add `CMD_SOURCE_HTTP = 3` to the `command_source_t` enum in `uart_handler.h`.

### 5.4 `captive_dns.c` — Minimal DNS server

Listens on UDP port 53. Responds to **every** DNS query with the ESP32's AP IP (`192.168.4.1`). This triggers the OS "sign in to network" popup on phones and desktops.

```c
// Core loop: receive DNS query, overwrite answer section with 192.168.4.1, send back
static void dns_server_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // bind to port 53 on 192.168.4.1
    // receive packet, patch the response with our IP, sendto client
}
```

---

## 6. AP/STA Provisioning Flow

```
Boot
 │
 ├─ config_init()          ← load all NVS config (existing)
 │
 ├─ WiFi credentials in NVS?
 │   ├─ YES → start STA mode → connect (30s timeout)
 │   │   ├─ Connected → start mDNS "gateway.local"
 │   │   │              start web_config_handler(WEB_MODE_STA)
 │   │   └─ Timeout   → fallback to AP mode (below)
 │   │
 │   └─ NO  → start AP mode
 │             SSID: "DA2-Gateway-{last4 of MAC}"
 │             Password: (open or configurable)
 │             start captive_dns_server()
 │             start web_config_handler(WEB_MODE_AP)
 │
 │  [User opens portal, sets WiFi credentials]
 │  POST /api/config → {wifi: {ssid:..., password:...}}
 │  → saved to NVS
 │  → restart ESP32 (or attempt reconnect inline)
```

---

## 7. CMakeLists.txt Changes

In **`main/CMakeLists.txt`**, add:

```cmake
# New web config handler sources
set(WEB_SRCS
    "../Application/Web_Config_Handler/src/web_server.c"
    "../Application/Web_Config_Handler/src/api_config.c"
    "../Application/Web_Config_Handler/src/api_status.c"
    "../Application/Web_Config_Handler/src/captive_dns.c"
)

idf_component_register(SRCS "DA2_esp.c"
                        # ... all existing sources ...
                        ${WEB_SRCS}
                    PRIV_REQUIRES
                        # ... all existing requires ...
                        esp_http_server    # ← ADD: web server (different from esp_http_client)
                        mdns               # ← ADD: mDNS for gateway.local
                        lwip               # already implied, make explicit for DNS socket
                    INCLUDE_DIRS
                        # ... all existing include dirs ...
                        "../Application/Web_Config_Handler/include"
                    EMBED_TXTFILES
                        # existing:
                        ${project_dir}/server_certs/ca_cert.pem
                        # ADD:
                        "../Application/Web_Config_Handler/web/index.html"
                    )
```

> **Important:** `esp_http_server` (for hosting) and `esp_http_client` (for outgoing HTTP telemetry) are **separate ESP-IDF components** — both can coexist.

---

## 8. Frontend Build (Vite SPA)

### Toolchain
```
Node.js  +  Vite  +  vite-plugin-singlefile
```

`vite-plugin-singlefile` inlines all JS and CSS into the HTML output — one self-contained file, ideal for `EMBED_TXTFILES`.

### `package.json` (in `Application/Web_Config_Handler/web/`)
```json
{
  "name": "da2-web-config",
  "scripts": {
    "dev":   "vite --port 5173",
    "build": "vite build"
  },
  "devDependencies": {
    "vite": "^5.0.0",
    "vite-plugin-singlefile": "^2.0.0"
  }
}
```

### `vite.config.js`
```js
import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    outDir: '.',           // output index.html directly into web/
    emptyOutDir: false,    // don't wipe src/
  },
});
```

### Tab Structure
The SPA mirrors the existing Python app. Start with 4 WAN-facing tabs:

| Tab | Endpoint | Python app equivalent |
|---|---|---|
| WiFi | `wifi` section of POST /api/config | `wifi_tab.py` |
| LTE | `lte` section | `lte_tab.py` |
| Server | `mqtt`/`http`/`coap` + `server_type` | `server_tab.py` |
| Status | GET /api/status | Console panel |

### Dev → Firmware workflow
```
npm run build           # outputs web/index.html
idf.py build            # EMBED_TXTFILES picks up the new index.html
idf.py flash monitor
```

---

## 9. Phased Implementation

### Phase 1 — Core Web Server (no AP/STA switching)
**Goal:** Serve the web page and read/write config over existing STA WiFi.

- [ ] Create `Application/Web_Config_Handler/` directory structure
- [ ] Implement `web_server.c`: httpd start/stop, serve hardcoded `"<h1>Hello</h1>"` first
- [ ] Implement `api_config.c`: `GET /api/config` returns JSON from NVS globals
- [ ] Implement `api_status.c`: `GET /api/status` returns basic state
- [ ] Add `CMD_SOURCE_HTTP` to `command_source_t` enum in `uart_handler.h`
- [ ] Update `main/CMakeLists.txt`: add sources, `esp_http_server`, `mdns` PRIV_REQUIRES
- [ ] Call `web_config_handler_start(WEB_MODE_STA)` in `DA2_esp.c` after WiFi connects
- [ ] Verify with `curl http://<device_ip>/api/config`

### Phase 2 — Captive Portal AP Mode
**Goal:** First-time setup works — phone connects to AP and gets the config page automatically.

- [ ] Implement `captive_dns.c`: UDP socket on port 53, reply all queries with `192.168.4.1`
- [ ] Add AP mode startup logic in `DA2_esp.c`: if no WiFi credentials in NVS → start AP
- [ ] Add STA fallback: 30s connection timeout → restart in AP mode
- [ ] Wire `captive_dns_server_start()` / `captive_dns_server_stop()` into the AP/STA state machine
- [ ] Test on Android (auto-opens portal) and iOS (banner notification)

### Phase 3 — mDNS
**Goal:** `http://gateway.local` resolves to device on any local network.

- [ ] Add `mdns_init()`, `mdns_hostname_set("gateway")`, `mdns_instance_name_set("DA2 Gateway")` after STA connects
- [ ] Test from Windows/macOS/Linux browser
- [ ] Add `mdns_service_add()` for HTTP port 80

### Phase 4 — Vite SPA Frontend
**Goal:** Real UI matching Python app tabs.

- [ ] Set up `Application/Web_Config_Handler/web/` as a Node.js project
- [ ] Implement WiFi tab: form → `POST /api/config` with `{wifi: {...}}`
- [ ] Implement LTE tab: form → `POST /api/config` with `{lte: {...}}`
- [ ] Implement Server tab: MQTT/HTTP/CoAP selector + fields
- [ ] Implement Status tab: polls `GET /api/status` every 5s, shows live state
- [ ] Add `npm run build` step to firmware build script (`build.sh`)
- [ ] Update `CMakeLists.txt` to use `EMBED_TXTFILES` with built `index.html`

### Phase 5 — Live Log (Optional)
**Goal:** Stream UART logs to browser in real time.

- [ ] Add WebSocket endpoint `WS /api/log` using `esp_websocket_server` component  
  (or implement over HTTP chunked transfer if WebSocket is too heavy)
- [ ] Pipe `uart_handler` receive buffer → WebSocket broadcast
- [ ] Add log panel to SPA (auto-scroll textarea)

---

## 10. Resource Budget

| Item | Size estimate |
|---|---|
| esp_http_server stack | ~8 KB (FreeRTOS task) |
| captive_dns_task stack | ~4 KB |
| HTML served per client | 200–350 KB (one-time, then JS/CSS cached) |
| SPA index.html in flash | 200–350 KB inside 7 MB OTA partition — **< 5%** |
| NVS usage | no change (config was already stored there) |
| RAM per HTTP connection | ~6 KB (httpd default, configurable) |

**No flash partition changes required. No RAM pressure concerns on ESP32-S3 with PSRAM.**

---

## 11. Files to Create / Modify

| Action | File |
|---|---|
| **Create** | `Application/Web_Config_Handler/include/web_config_handler.h` |
| **Create** | `Application/Web_Config_Handler/src/web_server.c` |
| **Create** | `Application/Web_Config_Handler/src/api_config.c` |
| **Create** | `Application/Web_Config_Handler/src/api_status.c` |
| **Create** | `Application/Web_Config_Handler/src/captive_dns.c` |
| **Create** | `Application/Web_Config_Handler/web/index.html` (initially placeholder) |
| **Create** | `Application/Web_Config_Handler/web/package.json` |
| **Create** | `Application/Web_Config_Handler/web/vite.config.js` |
| **Modify** | `main/CMakeLists.txt` — add sources, requires, EMBED_TXTFILES |
| **Modify** | `main/DA2_esp.c` — call `web_config_handler_start()` after WiFi init |
| **Modify** | `Application/Data_Communication_Handler/include/uart_handler.h` — add `CMD_SOURCE_HTTP` |
| **No change** | `partitions.csv` |
| **No change** | `Application/Config_Handler/src/config_handler.c` |
| **No change** | `Application/Config_Handler/src/config_load_save.c` |
