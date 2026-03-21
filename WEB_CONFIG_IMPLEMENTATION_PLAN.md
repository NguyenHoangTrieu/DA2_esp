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
    ├── package.json         ← npm manifest + dev/mock/build scripts
    ├── vite.config.js       ← viteSingleFile + proxy config
    ├── mock_server.js       ← Express mock API (dev-only, not in firmware)
    └── src/                 ← source files (not compiled by IDF, managed by Vite)
        ├── main.js          ← entry: top bar, mode switch, tab routing, status poll
        ├── api.js           ← fetch wrapper + Save/Load JSON file helpers
        ├── style.css        ← light theme CSS matching Python app palette
        ├── config-form.js   ← shared ConfigForm builder for BLE/LoRa/Zigbee
        ├── stack-data.js    ← bundled stack_id_map + default JSON configs
        └── tabs/
            ├── wifi.js       ← Basic & Advanced WiFi tab
            ├── lte.js        ← Basic & Advanced LTE tab
            ├── server.js     ← Basic & Advanced Server tab (MQTT/HTTP/CoAP)
            ├── interfaces.js ← Basic mode: read-only detected stacks info
            ├── ble.js        ← Advanced: full BLE JSON config builder
            ├── lora.js       ← Advanced: full LoRa JSON config builder
            ├── zigbee.js     ← Advanced: full Zigbee JSON config builder
            ├── firmware.js   ← Advanced: OTA URL inputs + flash log
            └── basic-module.js ← Basic mode: BLE/LoRa/Zigbee quick controls tab
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

---

## 12. Current Implementation Status

### ✅ Done — C Backend (Firmware Side)

| File | Status |
|---|---|
| `Application/Web_Config_Handler/include/web_config_handler.h` | ✅ Created |
| `Application/Web_Config_Handler/src/web_server.c` | ✅ Created |
| `Application/Web_Config_Handler/src/api_config.c` | ✅ Created — all 7 config sections, LAN proxy |
| `Application/Web_Config_Handler/src/api_status.c` | ✅ Created |
| `Application/Web_Config_Handler/src/captive_dns.c` | ✅ Created |
| `Application/MCU_LAN_Handler/include/mcu_lan_handler.h` | ✅ Modified — `CMD_SOURCE_HTTP = 3` added |
| `main/CMakeLists.txt` | ✅ Modified — all sources, deps, EMBED_TXTFILES added |
| `build.sh` | ✅ Modified — step 3 auto-builds web UI before `idf.py build` |

**Not yet done in firmware:**
- [ ] Call `web_config_handler_start()` in `main/DA2_esp.c` (see section 9 Phase 1)
- [ ] AP/STA provisioning state machine in `DA2_esp.c` (see section 9 Phase 2)

---

### ✅ Done — Frontend Vite Project (Web Side)

The frontend SPA matches the Python config app (DATN_config_app v4.0) UI structure:

| File | Status | Description |
|---|---|---|
| `web/package.json` | ✅ | npm manifest with dev/mock/build scripts |
| `web/vite.config.js` | ✅ | viteSingleFile plugin + proxy /api → :3001 |
| `web/mock_server.js` | ✅ | Express mock with all 6 endpoints |
| `web/index.html` | ✅ | Vite entry point (top bar + mode switch + panels) |
| `web/src/style.css` | ✅ | Light theme matching Python app color palette |
| `web/src/api.js` | ✅ | fetch wrapper + Save/Load JSON file helpers |
| `web/src/main.js` | ✅ | Top bar, Basic/Advanced mode, tab routing, 5s status poll |
| `web/src/config-form.js` | ✅ | Shared ConfigForm for BLE/LoRa/Zigbee (comm + functions accordion + JSON preview + actions) |
| `web/src/stack-data.js` | ✅ | Bundled stack_id_map + default JSON configs |
| `web/src/tabs/wifi.js` | ✅ | SSID/password/auth/enterprise (Basic & Advanced) |
| `web/src/tabs/lte.js` | ✅ | Basic (APN only) + Advanced (modem/comm/pins/timeout) |
| `web/src/tabs/server.js` | ✅ | MQTT/HTTP/CoAP sub-forms with server type selector |
| `web/src/tabs/interfaces.js` | ✅ | Basic mode: detected stacks read-only info |
| `web/src/tabs/ble.js` | ✅ | Advanced: full BLE JSON config builder |
| `web/src/tabs/lora.js` | ✅ | Advanced: full LoRa JSON config builder |
| `web/src/tabs/zigbee.js` | ✅ | Advanced: full Zigbee JSON config builder (+ cmd_type/cmd_code/async) |
| `web/src/tabs/firmware.js` | ✅ | WAN/LAN OTA URL inputs + flash log |
| `web/src/tabs/basic-module.js` | ✅ | Basic mode: JSON send + quick controls + connection + response |

**UI features matching Python app:**
- Basic/Advanced mode toggle (persisted in localStorage)
- Top bar: status indicator, hostname, Read Config, Save File, Load File
- Basic mode: WiFi, (conditional LTE), Server, Interfaces, dynamic module tabs
- Advanced mode: WiFi, LTE, Server, BLE, LoRa, Zigbee, Firmware
- BLE/LoRa/Zigbee: header (slot/preset/module ID/name), communication panel, functions accordion with GPIO/delay/timeout per item, real-time JSON preview, actions (generate/save/load/send), status panel
- Zigbee extras: cmd_type, cmd_code, resp_format, async badge
- Form validation with inline error messages
- Toast notifications for success/error feedback
- Responsive layout (1300px max, mobile-friendly)

---

## 13. Testing the Web UI Without MCU

The full Vite dev workflow works **entirely on your PC** using a local mock API server. No ESP32 is needed to develop and test the UI.

### How It Works

```
Browser (localhost:5173)
        │
        │  fetch('/api/config')
        ▼
Vite Dev Server (port 5173)
        │
        │  proxy: /api/* → localhost:3001
        ▼
Mock API Server (port 3001)   ← Node.js / Express, runs on PC
        │
        │  returns hardcoded JSON responses
        ▼
     (no MCU needed)
```

- Vite's built-in `server.proxy` redirects all `/api` calls to the mock server
- The mock server returns realistic JSON matching the exact API contract from section 4
- When the frontend POSTs config changes, the mock server logs them to the terminal
- Hot-reload works — edit any `.js` or `.css` file, browser updates instantly

---

### Setup — Vite Proxy Config

In `web/vite.config.js`:

```js
import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    outDir: '.',
    emptyOutDir: false,
  },
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://localhost:3001',
        changeOrigin: true,
      },
    },
  },
});
```

---

### Setup — Mock API Server

Create `web/mock_server.js` (not compiled into firmware — dev-only):

```js
// web/mock_server.js
// Run with: node mock_server.js
// Simulates the ESP32 REST API on localhost:3001

import express from 'express';
const app = express();
app.use(express.json());

// ── In-memory state (mirrors NVS globals on the MCU) ──────────────────────
let state = {
  internet_type: 0,
  server_type: 0,
  wifi: { ssid: 'TestNetwork', password: 'secret123', username: '', auth_mode: 0 },
  lte: {
    modem_name: 'A7600C1', apn: 'internet', username: '', password: '',
    comm_type: 0, auto_reconnect: true, reconnect_timeout_ms: 30000,
    max_reconnect_attempts: 0, pwr_pin: 11, rst_pin: 12
  },
  mqtt: {
    broker_uri: 'mqtt://demo.thingsboard.io:1883',
    device_token: 'demo_token_123',
    subscribe_topic: 'v1/devices/me/rpc/request/+',
    publish_topic: 'v1/devices/me/telemetry',
    attribute_topic: 'v1/devices/me/attributes'
  },
  http: { server_url: 'http://demo.server.io:8080/api', auth_token: '', port: 8080, use_tls: false, verify_server: false, timeout_ms: 10000 },
  coap: { host: 'demo.thingsboard.io', resource_path: '/api/v1/{token}/telemetry', device_token: '', port: 5683, use_dtls: false, ack_timeout_ms: 2000, max_retransmit: 4 }
};

let startTime = Date.now();

// ── GET /api/config ────────────────────────────────────────────────────────
app.get('/api/config', (req, res) => {
  console.log('[GET] /api/config');
  res.json(state);
});

// ── POST /api/config ───────────────────────────────────────────────────────
app.post('/api/config', (req, res) => {
  const body = req.body;
  console.log('[POST] /api/config', JSON.stringify(body, null, 2));

  // Merge each present section (mirrors what api_config.c does)
  if (body.wifi)          Object.assign(state.wifi, body.wifi);
  if (body.lte)           Object.assign(state.lte, body.lte);
  if (body.mqtt)          Object.assign(state.mqtt, body.mqtt);
  if (body.http)          Object.assign(state.http, body.http);
  if (body.coap)          Object.assign(state.coap, body.coap);
  if (body.internet_type !== undefined) state.internet_type = body.internet_type;
  if (body.server_type   !== undefined) state.server_type   = body.server_type;

  res.json({ ok: true, queued: 1, errors: 0 });
});

// ── GET /api/lan_config ────────────────────────────────────────────────────
app.get('/api/lan_config', (req, res) => {
  console.log('[GET] /api/lan_config');
  // Simulate a colon-separated LAN config string (like CFSC response)
  const mockLanData = 'CFBJ{"channel":11,"role":"peripheral","name":"DA2-BLE"}\nCFLJ{"sf":7,"bw":125,"freq":868}\nCFZJ{"pan_id":"0x1234","channel":15}';
  res.json({ ok: true, data: mockLanData });
});

// ── POST /api/lan_config ───────────────────────────────────────────────────
app.post('/api/lan_config', (req, res) => {
  console.log('[POST] /api/lan_config', JSON.stringify(req.body, null, 2));
  res.json({ ok: true });
});

// ── GET /api/status ────────────────────────────────────────────────────────
app.get('/api/status', (req, res) => {
  const uptime = Math.floor((Date.now() - startTime) / 1000);
  res.json({
    firmware_version: '1.1.1.2',
    wan_fw: '1.1.1.2',
    lan_fw: '1.0.0.1',
    internet_type: state.internet_type,
    server_type: state.server_type,
    wifi_connected: true,
    wifi_rssi: -58,
    internet_online: true,
    uptime_s: uptime,
    rtc: new Date().toLocaleDateString('en-GB') + '-' + new Date().toTimeString().slice(0,8),
    free_heap: 245760
  });
});

// ── POST /api/reboot ───────────────────────────────────────────────────────
app.post('/api/reboot', (req, res) => {
  console.log('[POST] /api/reboot — simulating reboot (resetting state uptime)');
  startTime = Date.now();
  res.json({ ok: true, message: 'Rebooting in 500ms' });
});

app.listen(3001, () => console.log('Mock API server running at http://localhost:3001'));
```

Add `express` to `package.json` devDependencies:
```json
{
  "name": "da2-web-config",
  "type": "module",
  "scripts": {
    "dev":   "vite --port 5173",
    "mock":  "node mock_server.js",
    "build": "vite build"
  },
  "devDependencies": {
    "vite": "^5.0.0",
    "vite-plugin-singlefile": "^2.0.0",
    "express": "^4.18.0"
  }
}
```

---

### Dev Workflow (Two Terminals)

```
Terminal 1 — start mock API:
  cd Application/Web_Config_Handler/web
  npm install
  npm run mock
  → Mock API server running at http://localhost:3001

Terminal 2 — start Vite dev server:
  cd Application/Web_Config_Handler/web
  npm run dev
  → Local:  http://localhost:5173
  → open browser at http://localhost:5173
```

- All `/api/*` calls from the browser are proxied to port 3001  
- State is held in memory — edits via the UI persist until the mock is restarted  
- No WiFi, no MCU, no flashing needed

---

### Firmware Build Workflow (When Ready to Flash)

```
1.  cd Application/Web_Config_Handler/web
    npm run build
    → Vite inlines all JS+CSS into web/index.html (single file, ~150-300 KB)

2.  cd DA2_esp
    idf.py build
    → CMake EMBED_TXTFILES picks up the built index.html
    → index.html bytes are linked into firmware binary as _binary_index_html_start

3.  idf.py flash monitor
    → Device serves the complete SPA from internal flash — no external files
```

---

### Testing Checklist (No MCU)

| Test | How |
|---|---|
| All tabs render correctly | Open `http://localhost:5173`, switch each tab |
| Config loads from API on page open | Check fields pre-filled after `GET /api/config` returns |
| WiFi save sends correct JSON | Save WiFi, check terminal 1 log for correct `{wifi:{...}}` |
| Server type switch shows correct sub-form | Change selector between MQTT/HTTP/CoAP |
| Status tab polls every 5s | Watch uptime counter increment in Status tab |
| LAN tab sends correct type+data | Send BLE JSON, check mock log |
| Reboot button | Click → mock logs `POST /api/reboot` |
| Responsive layout on mobile size | DevTools → toggle device toobar (375px width) |
