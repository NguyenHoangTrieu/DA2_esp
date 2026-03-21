# Web Config Portal — Complete Data Flow & Command Reference
## For WAN MCU (DA2_esp) + LAN MCU (DA2_esp_LAN)

---

## 1. System Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         USER INTERFACE                               │
│                                                                      │
│  [Browser SPA]              [Python Desktop App]                    │
│  http://gateway.local       COM port @ 115200                       │
│  POST /api/config   ◄──┐    CF...  CFSC  commands                  │
│  GET  /api/config   ──►├────────────────────────┐                   │
└────────────────────────┼────────────────────────┼───────────────────┘
                         │ HTTP (WiFi)             │ UART / USB Serial
                         ▼                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    WAN MCU  (DA2_esp / ESP32-S3)                     │
│                                                                      │
│  web_server.c ──► api_config.c ──►┐                                │
│                                    ├──► g_config_handler_queue ──► config_handler.c
│  uart_handler.c ──────────────────►│        (DA2_esp WAN side)     │
│  usb_handler.c ────────────────────┘                                │
│                                                                      │
│  Handles: WF / LT / MQ / SV / HP / CP / IN / ML commands           │
│                                                                      │
│  For ML commands → mcu_lan_handler_update_config()                  │
│  ─── SPI QSPI ──────────────────────────────────────────────────── │
└─────────────────────────────────────┬───────────────────────────────┘
                                      │ SPI (QSPI Slave/Master)
                                      │ GPIO8 = Data-Ready signal
                                      ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    LAN MCU  (DA2_esp_LAN / ESP32-S3)                 │
│                                                                      │
│  mcu_wan_handler ──► mcu_wan_config_callback() ──►                  │
│                       g_config_handler_queue ──► config_handler.c   │
│                           (DA2_esp_LAN side)                         │
│                                                                      │
│  Handles: CFBL / CFLR / CFZB / CFRS / CFFW commands                │
│  Routes to: BLE handler / LoRa handler / Zigbee handler             │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Existing Command Protocol (as used by Python App and UART)

### 2.1 READ — CFSC (Config Scan)

Sent by app to WAN MCU to read the full current configuration.

```
App → WAN MCU:   "CFSC\r\n"

WAN MCU → App:
  CFSC_RESP:START
  [GATEWAY_INFO]
  model=ESP32S3_IoT_Gateway
  firmware=v1.2.0
  hardware=HW_v2.0
  serial=GW2025001
  internet_status=ONLINE
  rtc_time=20/03/2026-14:30:00

  [WAN_CONFIG]
  internet_type=WIFI
  wifi_ssid=MyNetwork
  wifi_password=***HIDDEN***
  wifi_username=
  wifi_auth_mode=0
  lte_apn=v-internet
  lte_username=
  lte_password=***HIDDEN***
  lte_comm_type=USB
  lte_max_retries=5
  lte_timeout_ms=30000
  lte_auto_reconnect=true
  lte_modem_name=A7600C1
  lte_pwr_pin=WK
  lte_rst_pin=PE
  server_type=MQTT
  mqtt_broker=mqtt://demo.thingsboard.io:1883
  mqtt_pub_topic=v1/devices/me/telemetry
  mqtt_sub_topic=v1/devices/me/rpc/request/+
  mqtt_device_token=***HIDDEN***
  mqtt_attribute_topic=v1/devices/me/attributes
  stack_wan_id=100

  [LAN_CONFIG]
  stack1_id=002
  stack2_id=003
  rs485_baud_rate=115200
  stack1_json_len=1248
  stack2_json_len=987

  CFSC_RESP:END
```

**How the LAN section works:**  
The WAN MCU calls `mcu_lan_handler_request_config_async()` which sends a `CONFIG_REQ_LAN_CONFIG` request over SPI to the LAN MCU and waits up to 3 seconds. The LAN MCU responds with `key=value|key=value|...` pipe-delimited string which the WAN MCU then forwards to UART/USB/Web.

---

### 2.2 WRITE — WAN Config Commands (handled by WAN MCU only)

All write commands arrive at WAN MCU → `g_config_handler_queue` → `config_handler.c`.  
Commands are prefixed with **`CF`** when sent from app via UART. The UART handler strips the first `CF` before queueing.

> **Key difference for Web API:**  
> The web API will receive full JSON, parse it, then build the exact same wire-format command strings and send them into `g_config_handler_queue` directly — bypassing UART entirely.

---

#### 2.2.1 WiFi Configuration
```
Wire format (UART/USB):   CF + WF:SSID:PASSWORD:AUTH_MODE
                  or:     CF + WF:SSID:PASSWORD:USERNAME:AUTH_MODE

Examples:
  CFWF:MyNetwork:secret123:PERSONAL
  CFWF:CorpNet:secret123:user@corp.com:ENTERPRISE

Parsed by: config_parse_wifi()  (config_handler.c)
Stored in: g_wifi_ctx  (wifi_connect.c)
Saved to:  NVS namespace "gateway_cfg" key "wifi_cfg"
Action:    wifi_connect_task restart with new credentials

Fields:
  SSID        → g_wifi_ctx.ssid        (max 64 chars)
  PASSWORD    → g_wifi_ctx.pass        (max 64 chars)
  USERNAME    → g_wifi_ctx.username    (max 64 chars, Enterprise only)
  AUTH_MODE   → PERSONAL(0) | ENTERPRISE(1)
```

#### 2.2.2 LTE Configuration
```
Wire format:
  CFLT:MODEM_NAME:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:RECONNECT_TIMEOUT_MS:MAX_RECONNECT:PWR_PIN:RST_PIN

Example:
  CFLT:A7600C1:v-internet:user:pass:USB:true:30000:0:WK:PE

Parsed by: config_parse_lte()  (config_handler.c)
Stored in: g_lte_ctx  (lte_connect.c)
Saved to:  NVS key "lte_cfg"
Action:    lte_connect_task restart with new credentials

Fields:
  MODEM_NAME       → "A7600C1", "SIM7600E", etc.        (max 32 chars)
  APN              → operator APN string                  (max 64 chars)
  USERNAME         → PPP username (optional, can be "")  (max 32 chars)
  PASSWORD         → PPP password (optional, can be "")  (max 32 chars)
  COMM_TYPE        → "USB" | "UART"
  AUTO_RECONNECT   → "true" | "false"
  RECONNECT_TIMEOUT_MS → integer (e.g. 30000)
  MAX_RECONNECT    → integer (0 = unlimited)
  PWR_PIN          → "WK"=TCA_WAKE(11), "PE"=TCA_PERST(12), "01".."11"
  RST_PIN          → same format as PWR_PIN
```

#### 2.2.3 Internet Type Selection
```
Wire format:   CFIN:WIFI    or   CFIN:LTE   or   CFIN:ETHERNET

Parsed by:  config_parse_internet()
Stored in:  g_internet_type  (config_handler.c)
Saved to:   NVS key "inet_type"
Action:     stops current internet handler, starts new one
```

#### 2.2.4 Server Type Selection
```
Wire format:   CFSV:0   (MQTT)
               CFSV:1   (CoAP)
               CFSV:2   (HTTP)

Parsed by:  config_parse_server_type()
Stored in:  g_server_type
Saved to:   NVS key "srv_type"
Action:     stops current server handler, starts new one

Python builder:  build_server_type_cmd(server_type: int) -> str
```

#### 2.2.5 MQTT Configuration
```
Wire format:
  CFMQ:BROKER_URI|DEVICE_TOKEN|SUBSCRIBE_TOPIC|PUBLISH_TOPIC|ATTRIBUTE_TOPIC

Example:
  CFMQ:mqtt://demo.thingsboard.io:1883|myToken123|v1/devices/me/rpc/request/+|v1/devices/me/telemetry|v1/devices/me/attributes

Separator: | (pipe) — avoids collision with : used in URIs
Parsed by: config_parse_mqtt()
Stored in: g_mqtt_ctx  (mqtt_handler.c)
Saved to:  NVS key "mqtt_cfg"

Python builder:  build_mqtt_cmd(broker, token, sub, pub, attr)
```

#### 2.2.6 HTTP Server Configuration
```
Wire format:
  CFHP:URL|AUTH_TOKEN|PORT|USE_TLS|VERIFY_SERVER|TIMEOUT_MS

Example:
  CFHP:http://demo.thingsboard.io:8080/api/v1/{token}/telemetry|myToken|8080|0|0|10000

Separator: | (pipe)
Parsed by: config_parse_http()
Stored in: g_http_cfg  (config_handler.c)
Saved to:  NVS key "http_cfg"

Python builder:  build_http_cmd(url, auth_token, port, use_tls, verify_server, timeout_ms)
```

#### 2.2.7 CoAP Server Configuration
```
Wire format:
  CFCP:HOST|RESOURCE_PATH|DEVICE_TOKEN|PORT|USE_DTLS|ACK_TIMEOUT_MS|MAX_RETRANSMIT

Example:
  CFCP:demo.thingsboard.io|/api/v1/{token}/telemetry|myToken|5683|0|2000|4

Separator: | (pipe)
Parsed by: config_parse_coap()
Stored in: g_coap_cfg  (config_handler.c)
Saved to:  NVS key "coap_cfg"

Python builder:  build_coap_cmd(host, resource_path, token, port, use_dtls, ack_timeout_ms, max_retransmit)
```

---

### 2.3 WRITE — LAN Pass-Through Commands (WAN MCU routes to LAN MCU)

These commands have their `CF` prefix sent from app → WAN MCU, then WAN MCU forwards the **full command string** (including `CF` prefix) to LAN MCU via SPI using `mcu_lan_handler_update_config()`.

The config type tag in `config_handler.h` (WAN side) is `CONFIG_TYPE_MCU_LAN = 5` — prefix `"ML"`.

```
App → WAN MCU → "CF" + "ML:" + <LAN command string>
WAN MCU unpacks "ML:" payload → calls mcu_lan_handler_update_config(payload)
SPI transfer → LAN MCU mcu_wan_config_callback(payload)
LAN MCU enqueues to g_config_handler_queue (LAN side)
LAN config_handler.c processes based on LAN config_type_t
```

#### 2.3.1 BLE Module JSON Config
```
Full command sent to WAN MCU:
  CFML:CFBL:JSON:{...json...}

What WAN MCU strips and forwards to LAN MCU:
  CFBL:JSON:{...json...}

LAN MCU config_parse_type() result: CONFIG_UPDATE_BLE_JSON
Handler: config_parse_ble_json()  (config_handler_ble_commands.c)
Action:  saves JSON to NVS for stack_id, applies to BLE module via UART
```

#### 2.3.2 BLE Module AT Command
```
Full command:
  CFML:CFBL:<stack_slot>:<function_name>[:<param>]

What LAN MCU receives:
  CFBL:<stack_slot>:<function_name>[:<param>]

LAN MCU config_parse_type() result: CONFIG_UPDATE_BLE_CMD
Handler: config_parse_ble_command()  (config_handler_ble_commands.c)

Examples:
  CFBL:S1:MODULE_SW_RESET
  CFBL:S1:MODULE_SET_NAME:MyBLEDevice
  CFBL:S2:MODULE_START_BROADCAST
```

#### 2.3.3 LoRa Module JSON Config
```
Full command:
  CFML:CFLR:JSON:{...json...}

LAN MCU receives:   CFLR:JSON:{...json...}
LAN type:           CONFIG_UPDATE_LORA_JSON
Handler:            config_parse_lora_json()  (config_handler_lora_commands.c)
```

#### 2.3.4 LoRa Module AT Command
```
Full command:
  CFML:CFLR:<stack_slot>:<function_name>[:<param>]

LAN MCU receives:   CFLR:<stack_slot>:<function_name>[:<param>]
LAN type:           CONFIG_UPDATE_LORA_CMD
Handler:            config_parse_lora_command()  (config_handler_lora_commands.c)

Examples:
  CFLR:S1:MODULE_HW_RESET
  CFLR:S1:MODULE_SET_REGION:EU868
  CFLR:S1:MODULE_JOIN
```

#### 2.3.5 Zigbee Module JSON Config
```
Full command:
  CFML:CFZB:JSON:{...json...}

LAN MCU receives:   CFZB:JSON:{...json...}
LAN type:           CONFIG_UPDATE_ZIGBEE_JSON
Handler:            config_parse_zigbee_json()  (config_handler_zigbee_commands.c)
```

#### 2.3.6 Zigbee Module AT/Hex Command
```
Full command:
  CFML:CFZB:<stack_slot>:<function_name>[:<param>]

LAN MCU receives:   CFZB:<stack_slot>:<function_name>[:<param>]
LAN type:           CONFIG_UPDATE_ZIGBEE_CMD
Handler:            config_parse_zigbee_command()  (config_handler_zigbee_commands.c)

Examples:
  CFZB:S1:MODULE_START_NETWORK
  CFZB:S1:MODULE_SET_PERMIT_JOIN:60
  CFZB:S1:MODULE_ZCL_SEND_CONTROL_CMD
```

#### 2.3.7 RS485 Baud Rate
```
Full command:
  CFML:CFRS:BR:115200

LAN MCU receives:   CFRS:BR:115200
LAN type:           CONFIG_UPDATE_RS485
Handler:            config_parse_rs485_baud()
Valid baud rates:   9600, 19200, 38400, 57600, 115200
```

#### 2.3.8 Firmware Update (FOTA) — Both MCUs
```
One command sent:
  CFML:CFFW

Flow:
  WAN MCU receives: raw_data="ML:CFFW", type=CONFIG_TYPE_MCU_LAN
    → Detects "CFFW" is a firmware update (is_fota=true)
    → Strips "ML:" prefix
    → Sends "CFFW" to LAN MCU via SPI
  LAN MCU receives: "CFFW"
    → Detects as FOTA command
    → Triggers fota_lan_handler_task_start()

Firmware URLs: hardcoded in both WAN and LAN firmware (not user-configurable)
  → Prevents users from flashing incorrect firmware versions
  → Both MCUs update simultaneously
```

---

## 3. Complete Data Flow Diagrams

### 3.1 READ Flow — Web equivalent of CFSC

```
Browser                  WAN MCU                    LAN MCU
  │                         │                           │
  ├──GET /api/config────────►│                           │
  │                         │── mcu_lan_handler_        │
  │                         │   request_config_async()──►│
  │                         │   (SPI, 3s timeout)        │
  │                         │                           │── build key=value
  │                         │                           │   response string
  │                         │◄──────────────────────────│
  │                         │                           │
  │                         │  reads g_wifi_ctx,        │
  │                         │  g_lte_ctx, g_mqtt_ctx,   │
  │                         │  g_http_cfg, g_coap_cfg   │
  │                         │  (all thread-safe via mutex)
  │                         │                           │
  │◄─JSON response──────────│                           │
  │  {wan:{...}, lan:{...}} │                           │
```

### 3.2 WRITE Flow — WAN Config (WiFi/LTE/Server)

```
Browser               WAN MCU api_config.c     WAN config_handler.c
  │                         │                           │
  ├─POST /api/config────────►│                           │
  │  {wifi:{ssid,pass}}     │  malloc config_command_t  │
  │                         │  .type = CONFIG_TYPE_WIFI │
  │                         │  .source = CMD_SOURCE_HTTP│
  │                         │  .raw_data = "WF:ssid:pass:PERSONAL"
  │                         │  xQueueSend(g_config_handler_queue)
  │                         │──────────────────────────►│
  │                         │                           │  config_parse_wifi()
  │                         │                           │  config_update_wifi_safe()
  │                         │                           │  save_wifi_config_to_nvs()
  │                         │                           │  restart wifi_connect_task
  │◄─{ok:true}──────────────│                           │
```

### 3.3 WRITE Flow — LAN Pass-Through (BLE/LoRa/Zigbee)

```
Browser            WAN MCU api_config.c   WAN config_handler.c   WAN mcu_lan_handler   LAN mcu_wan_handler   LAN config_handler
  │                    │                        │                        │                      │                     │
  ├─POST /api/config──►│                        │                        │                      │                     │
  │ {ble:{stack:"S1",  │  build "ML:" command   │                        │                      │                     │
  │  json:{...}}}      │  type=CONFIG_TYPE_MCU_LAN                       │                      │                     │
  │                    │────────────────────────►│                        │                      │                     │
  │                    │                        │  extract ML payload     │                      │                     │
  │                    │                        │  "CFBL:JSON:{...}"      │                      │                     │
  │                    │                        │  mcu_lan_handler_       │                      │                     │
  │                    │                        │  update_config(payload) │                      │                     │
  │                    │                        │────────────────────────►│                      │                     │
  │                    │                        │                        │ SPI transfer +         │                     │
  │                    │                        │                        │ GPIO8 data-ready pulse │                     │
  │                    │                        │                        │──────────────────────►│                     │
  │                    │                        │                        │                      │  mcu_wan_config_     │
  │                    │                        │                        │                      │  callback()          │
  │                    │                        │                        │                      │─────────────────────►│
  │                    │                        │                        │                      │                     │  config_parse_type()
  │                    │                        │                        │                      │                     │  = CONFIG_UPDATE_BLE_JSON
  │                    │                        │                        │                      │                     │  config_parse_ble_json()
  │                    │                        │                        │                      │                     │  save to NVS
  │                    │                        │                        │                      │                     │  send AT cmds to UART
  │◄─{ok:true}─────────│                        │                        │                      │                     │
```

---

## 4. Queue Architecture (What to Reuse vs. What to Add)

### WAN MCU Queue

```c
// EXISTING — do NOT change
QueueHandle_t g_config_handler_queue;   // holds config_command_t* (pointer-to-heap)
// Queue depth: 20 (CONFIG_QUEUE_SIZE)
// Item size:   sizeof(config_command_t*)   ← IMPORTANT: queue holds POINTER, not struct
```

```c
// EXISTING command_source_t enum (mcu_lan_handler.h)
typedef enum {
    CMD_SOURCE_MQTT    = 0,
    CMD_SOURCE_UART    = 1,
    CMD_SOURCE_USB     = 2,
    CMD_SOURCE_UNKNOWN = 0xFF
} command_source_t;

// ADD one new value:
    CMD_SOURCE_HTTP    = 3,   // From web browser via esp_http_server
```

### LAN MCU Queue

```c
// EXISTING — do NOT change
QueueHandle_t g_config_handler_queue;   // holds config_command_t* (pointer-to-heap)
// config_source_t values: CONFIG_SOURCE_WAN_MCU=0, _UART=1, _USB=2
// All web-originated commands arrive here as CONFIG_SOURCE_WAN_MCU
// (web → WAN MCU → SPI → LAN MCU — indistinguishable from UART path at LAN side)
```

---

## 5. Web `api_config.c` — What Each JSON Section Must Build

The POST body arrives as JSON. For each section present, the handler builds the exact wire-format command string and enqueues it to `g_config_handler_queue`.

```
POST /api/config body section → wire-format command string → config_type_t

"internet_type"   →  "IN:WIFI"  or "IN:LTE"           → CONFIG_TYPE_INTERNET
"wifi"            →  "WF:ssid:pass:PERSONAL"           → CONFIG_TYPE_WIFI
"lte"             →  "LT:modem:apn:user:pass:USB:..."  → CONFIG_TYPE_LTE
"server_type"     →  "SV:0"  (0=MQTT, 1=CoAP, 2=HTTP) → CONFIG_TYPE_SERVER
"mqtt"            →  "MQ:broker|token|sub|pub|attr"    → CONFIG_TYPE_MQTT
"http"            →  "HP:url|token|port|tls|verify|ms" → CONFIG_TYPE_HTTP
"coap"            →  "CP:host|path|token|port|dtls|ack|rtx" → CONFIG_TYPE_COAP
"ble_json"        →  "ML:CFBL:JSON:{...}"              → CONFIG_TYPE_MCU_LAN
"ble_cmd"         →  "ML:CFBL:S1:FUNC_NAME[:param]"   → CONFIG_TYPE_MCU_LAN
"lora_json"       →  "ML:CFLR:JSON:{...}"              → CONFIG_TYPE_MCU_LAN
"lora_cmd"        →  "ML:CFLR:S1:FUNC_NAME[:param]"   → CONFIG_TYPE_MCU_LAN
"zigbee_json"     →  "ML:CFZB:JSON:{...}"              → CONFIG_TYPE_MCU_LAN
"zigbee_cmd"      →  "ML:CFZB:S1:FUNC_NAME[:param]"   → CONFIG_TYPE_MCU_LAN
"rs485"           →  "ML:CFRS:BR:115200"               → CONFIG_TYPE_MCU_LAN
"fota"            →  "ML:CFFW"  (one command, both MCUs) → CONFIG_TYPE_MCU_LAN
```

> **Note on `raw_data` content:**  
> The WAN `config_handler.c` already strips the `CF` prefix for UART commands.  
> For the web handler, build `raw_data` **without** the `CF` prefix — pass it exactly like `uart_handler.c` does (e.g. `"WF:ssid:pass:PERSONAL"`, not `"CFWF:ssid:pass:PERSONAL"`).

---

## 6. GET /api/config — JSON Response Mapping

```c
// Sources of each field:
// (all already available in WAN MCU globals after config_init())

"internet_type"  ← g_internet_type  (0=WIFI, 1=LTE, 2=ETH)
"server_type"    ← g_server_type    (0=MQTT, 1=CoAP, 2=HTTP)

"wifi.ssid"      ← g_wifi_ctx.ssid
"wifi.password"  ← "" (never expose, always redact)
"wifi.username"  ← g_wifi_ctx.username
"wifi.auth_mode" ← g_wifi_ctx.auth_mode

"lte.modem_name" ← g_lte_ctx.modem_name
"lte.apn"        ← g_lte_ctx.apn
"lte.username"   ← g_lte_ctx.username
"lte.password"   ← "" (redact)
"lte.comm_type"  ← g_lte_ctx.comm_type  (0=USB, 1=UART)
"lte.auto_reconnect" ← g_lte_ctx.auto_reconnect
"lte.reconnect_timeout_ms" ← g_lte_ctx.reconnect_timeout_ms
"lte.max_reconnect_attempts" ← g_lte_ctx.max_reconnect_attempts
"lte.pwr_pin"    ← g_lte_ctx.pwr_pin  (uint8_t → convert to "WK"/"PE"/"01")
"lte.rst_pin"    ← g_lte_ctx.rst_pin

"mqtt.broker_uri"        ← g_mqtt_ctx.broker_uri
"mqtt.device_token"      ← "" (redact)
"mqtt.subscribe_topic"   ← g_mqtt_ctx.subscribe_topic
"mqtt.publish_topic"     ← g_mqtt_ctx.publish_topic
"mqtt.attribute_topic"   ← g_mqtt_ctx.attribute_topic

"http.server_url"    ← g_http_cfg.server_url
"http.auth_token"    ← "" (redact)
"http.port"          ← g_http_cfg.port
"http.use_tls"       ← g_http_cfg.use_tls
"http.verify_server" ← g_http_cfg.verify_server
"http.timeout_ms"    ← g_http_cfg.timeout_ms

"coap.host"            ← g_coap_cfg.host
"coap.resource_path"   ← g_coap_cfg.resource_path
"coap.device_token"    ← "" (redact)
"coap.port"            ← g_coap_cfg.port
"coap.use_dtls"        ← g_coap_cfg.use_dtls
"coap.ack_timeout_ms"  ← g_coap_cfg.ack_timeout_ms
"coap.max_retransmit"  ← g_coap_cfg.max_retransmit

// LAN section — from SPI request to LAN MCU (same as CFSC):
"lan.stack1_id"       ← from LAN config response
"lan.stack2_id"       ← from LAN config response
"lan.rs485_baud_rate" ← from LAN config response
"lan.stack1_json_len" ← from LAN config response
"lan.stack2_json_len" ← from LAN config response
```

All config reads are **thread-safe** through the existing mutex:
```c
config_get_wifi_safe(&wifi_cfg);
config_get_lte_safe(&lte_cfg);
config_get_mqtt_safe(&mqtt_cfg);
// g_http_cfg / g_coap_cfg: protect with g_config_context_mutex directly
```

---

## 7. GET /api/status — Live State Sources

```c
"wifi_connected"  ← wifi_get_connection_status() == 1
"wifi_rssi"       ← esp_wifi_sta_get_rssi() (only when connected)
"lte_connected"   ← lte_connect_task running + PPP up
"internet_online" ← mcu_lan_handler_get_internet_status() == INTERNET_STATUS_ONLINE
"mqtt_connected"  ← mqtt_handler task state (add mqtt_is_connected() getter)
"server_type"     ← g_server_type (0/1/2)
"firmware_version" ← WAN_FW_VERSION string ("1.1.1")
"uptime_s"        ← esp_timer_get_time() / 1000000
```

---

## 8. New Files to Write

### 8.1 `web_config_handler.h` — Public API
```c
typedef enum { WEB_MODE_AP, WEB_MODE_STA } web_server_mode_t;

void web_config_handler_start(web_server_mode_t mode);
void web_config_handler_stop(void);
```

### 8.2 `web_server.c` — httpd lifecycle + route table
Registers all `httpd_uri_t` routes. Embeds `index_html` via `EMBED_TXTFILES`.  
For AP mode: also starts `captive_dns` and adds `"/*"` catch-all redirect to `/`.

### 8.3 `api_config.c` — Core logic
Must implement two handlers:

**`api_config_get_handler()`**
```
1. config_get_wifi_safe()     → populate JSON "wifi" object
2. config_get_lte_safe()      → populate JSON "lte" object
3. config_get_mqtt_safe()     → populate JSON "mqtt" object
4. mutex-lock → copy g_http_cfg, g_coap_cfg → unlock
5. mcu_lan_handler_request_config_async() → populate JSON "lan" object
6. JSON serialize → httpd_resp_send()
```

**`api_config_post_handler()`**
```
1. httpd_req_recv() → body buffer (max CONFIG_CMD_MAX_LEN)
2. cJSON_Parse(body) — use ESP-IDF's cJSON (already bundled)
3. For each recognized JSON key:
   a. Build wire-format string (see section 5 mapping table)
   b. malloc config_command_t
   c. set .type, .source = CMD_SOURCE_HTTP, .data_len, copy to .raw_data
   d. xQueueSend(g_config_handler_queue, &cmd_ptr, timeout)
4. cJSON_Delete()
5. Send {"ok":true} or {"ok":false,"error":"..."}
```

### 8.4 `api_status.c` — Live status + reboot
Two endpoints:
- `GET /api/status` → build JSON from live state getters
- `POST /api/reboot` → send `{ok:true}` → `vTaskDelay(500ms)` → `esp_restart()`

### 8.5 `captive_dns.c` — UDP DNS server (AP mode only)
```
1. socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
2. bind to 0.0.0.0:53
3. Loop: recvfrom() → parse DNS query → patch answer → sendto() with 192.168.4.1
   (Copy QR, OPCODE from query header; set AA, RA bits; write A record)
```

---

## 9.  `main/CMakeLists.txt` Diff (exact additions)

```cmake
# === ADD new source files ===
set(WEB_SRCS
    "../Application/Web_Config_Handler/src/web_server.c"
    "../Application/Web_Config_Handler/src/api_config.c"
    "../Application/Web_Config_Handler/src/api_status.c"
    "../Application/Web_Config_Handler/src/captive_dns.c"
)

# === ADD to idf_component_register ===
SRCS:
    ${WEB_SRCS}                                         # ← ADD

PRIV_REQUIRES:
    esp_http_server                                     # ← ADD (host, NOT client)
    mdns                                                # ← ADD
    lwip                                                # ← ADD explicit

INCLUDE_DIRS:
    "../Application/Web_Config_Handler/include"         # ← ADD

EMBED_TXTFILES:
    # existing:
    ${project_dir}/server_certs/ca_cert.pem
    # ADD:
    "../Application/Web_Config_Handler/web/index.html"  # ← ADD
```

---

## 10.  `DA2_esp.c` Integration Points

```c
// In DA2_esp.h — ADD include:
#include "web_config_handler.h"

// In DA2_esp.c — ADD to wifi connected event handler:
//   (after wifi_connect reports IP assigned)
mdns_init();
mdns_hostname_set("gateway");
mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
web_config_handler_start(WEB_MODE_STA);

// In DA2_esp.c — ADD for first-boot AP mode:
//   (after config_init(), if g_wifi_ctx.ssid[0] == '\0')
wifi_ap_start("DA2-Gateway-XXXX");
web_config_handler_start(WEB_MODE_AP);

// In mcu_lan_handler.h — ADD to command_source_t:
CMD_SOURCE_HTTP = 3,    // ← ADD
```

---

## 11. Summary: What Already Exists vs. What to Write

| Component | Status | Notes |
|---|---|---|
| `g_config_handler_queue` (WAN) | ✅ Exists | Reuse as-is |
| `config_command_t` struct (WAN) | ✅ Exists | Add `CMD_SOURCE_HTTP` only |
| `config_parse_wifi/lte/mqtt/http/coap` | ✅ Exists | Called by web handler same way |
| `config_get_wifi/lte/mqtt_safe()` | ✅ Exists | Used in GET /api/config |
| `save_*_config_to_nvs()` | ✅ Exists | Called by config_handler after parsing |
| `mcu_lan_handler_update_config()` | ✅ Exists | Used for ML pass-through |
| `g_config_handler_queue` (LAN) | ✅ Exists | Untouched |
| LAN `config_parse_ble/lora/zigbee_*` | ✅ Exists | Untouched |
| `web_config_handler.h` | ❌ Write | Minimal, ~20 lines |
| `web_server.c` | ❌ Write | httpd start/stop/routes/embed |
| `api_config.c` | ❌ Write | Core of the work — GET+POST handlers |
| `api_status.c` | ❌ Write | Status JSON + reboot |
| `captive_dns.c` | ❌ Write | UDP DNS, ~100 lines |
| `index.html` (Vite SPA built output) | ❌ Write | Frontend work |
| `CMakeLists.txt` additions | ❌ Modify | ~10 lines |
| `DA2_esp.c` integration calls | ❌ Modify | ~10 lines |
| `command_source_t` enum addition | ❌ Modify | 1 line |
