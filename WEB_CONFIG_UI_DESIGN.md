# Web Config Portal — Complete UI/UX Design Reference
## Port from Python App (DATN_config_app v4.0) → Browser SPA

---

## 0. Quick Summary of Differences from Python App

| Aspect | Python App | Web App |
|---|---|---|
| Serial connection bar | Hardware COM port picker + baud selector + Scan | **Remove** — web is HTTP, always "connected" |
| UART Log panel | Raw byte display, RX/TX log | **Remove** (not needed, hard to implement) |
| Console Log panel | Debug/info text log | **Remove** (not needed) |
| Mode selector (Basic/Advanced) | Checkbox toggle | **Keep** — two distinct views |
| Read Config button | Sends `CFSC` over UART | `GET /api/config` HTTP request |
| Save File / Load File | JSON file I/O on desktop | Keep Save/Load (download JSON / import JSON) |
| Action: Set buttons | `serial_manager.send(cmd)` | `POST /api/config` with JSON body |
| Status bar | Text "Ready" + version | Keep |
| Firmware tab | Runs `flash_WAN.bat` via `subprocess` | **OTA URL input only** — no local flash script |
| Window size | `1300x800` fixed | Responsive layout |
| Theme | ttk 'clam' + custom | CSS design system (match color palette) |

### Color Palette (from Python styles)
- Background: `#F5F5F5`
- Label accent (blue): `#1565C0`
- Success green: `#4CAF50`
- Error red: `#F44336`
- Warning orange: `#FF9800`
- Muted text: `#757575`
- Hint text: `#888888`
- JSON preview bg: `#FAFAFA`
- Firmware log bg (dark): `#1E1E1E` fg `#CCCCCC`
- Font: Segoe UI (UI), Consolas (code/monospace)

---

## 1. Top-Level App Layout

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  🔌 ESP32 Gateway Configuration Tool                            v4.0.0 ©2024 │
├──────────────────────────────────────────────────────────────────────────────┤
│  TOP BAR                                                                      │
│  [● Connected: gateway.local]  [📖 Read Config] [💾 Save File] [📂 Load File]│
│  Status: Ready                                                               │
├──────────────────────────────────────────────────────────────────────────────┤
│  MODE SELECTOR                                                               │
│  ☐ Advanced Mode                                                             │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   CONTENT AREA (switches between Basic Panel and Advanced Panel)            │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 1.1 Top Bar (replaces Connection Bar)

The Python app's Connection Bar is replaced with a simpler **Status Bar** in the web app since there is no serial port to manage.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ● Connected  gateway.local    [📖 Read Config]  [💾 Save File]  [📂 Load]  │
└─────────────────────────────────────────────────────────────────────────────┘
```

| Element | Behavior | Python Equivalent |
|---|---|---|
| Status indicator (●) | Green circle = HTTP reachable; red = unreachable. Polls `GET /api/status` every 5s | `status_indicator` canvas oval |
| "gateway.local" | This browser's `window.location.hostname` (mDNS auto-resolved) | Port combo box |
| `📖 Read Config` | `GET /api/config` → fills all form fields | `_read_config()` → sends `CFSC` |
| `💾 Save File` | Downloads current form state as `.json` file | `_save_to_file()` |
| `📂 Load File` | File picker → imports JSON → populates form | `_load_from_file()` |
| Status text | "Ready" / "Sending…" / "Config loaded" | `self.status_label` |
| Version label | Right side: `v4.0.0 | © 2024` | `version_label` |

### 1.2 Mode Selector Row

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ☐ Advanced Mode                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

- Unchecked → show **Basic Panel** (default)  
- Checked → show **Advanced Panel**  
- State persists in `localStorage`

---

## 2. BASIC MODE Panel

The Basic Panel has **4 fixed tabs** always visible:
1. `📶 WiFi`
2. `🌐 Server`
3. `🔌 Interfaces`
4. Dynamic LAN module tabs added at runtime (BLE/LoRa/Zigbee based on detected stacks)

> **Note:** The LTE tab in basic mode is conditionally shown based on the gateway's WAN adapter ID (`stack_wan_id`). If the gateway is WiFi-only, the LTE tab is hidden. The web app should show/hide it based on the `GET /api/config` response field `wan.stack_wan_id`.

```
┌─ 📋 BASIC CONFIGURATION ──────────────────────────────────────────────────┐
│  [📶 WiFi]  [🌐 Server]  [🔌 Interfaces]  [🔷 BLE Stack 1]  ...          │
│                                                                            │
│  (tab content see sections below)                                         │
└───────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Basic — WiFi Tab

```
┌─ WiFi Settings ───────────────────────────────────────────┐
│  SSID:       [_________________________________]           │
│  Password:   [***********************] [☐ Show]          │
│  Auth Mode:  [PERSONAL ▼]                                 │
│  Username:   [_________________________________]  ← only when ENTERPRISE
└──────────────────────────────────────────────────────────┘
                                       [Set WiFi Config ✅]
```

| Field | Widget | Default | Validation |
|---|---|---|---|
| SSID | Text input | `""` | Required, non-empty |
| Password | Password input with Show toggle | `""` | Optional |
| Auth Mode | Select: `PERSONAL` / `ENTERPRISE` | `PERSONAL` | — |
| Username | Text input (shown only when ENTERPRISE) | `""` | Required when ENTERPRISE |

**Behavior:**
- Clicking `Set WiFi Config` sends: `POST /api/config` with `{ "wifi": { "ssid": "...", "password": "...", "auth_mode": "PERSONAL", "username": "" } }`
- The API internally sends `CFWF:SSID:PASS:AUTH` then after 1s `CFIN:WIFI`

### 2.2 Basic — LTE Tab (conditional)

Shown only when `config.wan.stack_wan_id` maps to an LTE-capable adapter in `stack_id_map`.

```
┌──────────────────────────────────────────────────────────┐
│  LTE Module:  A7600C1  (shown read-only from stack map)  │
├─ LTE Settings ───────────────────────────────────────────┤
│  APN:        [internet_____________________________]      │
│  Username:   [_____________________________________]      │
│  Password:   [***********************] [☐ Show]          │
└──────────────────────────────────────────────────────────┘
                                          [Set LTE Config ✅]
```

**Note:** Advanced fields (comm type, pins, timeouts) are in Advanced Mode → LTE Tab. Basic mode only shows APN/username/password.

**Behavior:** Sends `CFLT:MODEM:APN:USER:PASS:USB:true:30000:0:WK:PE` then after 1s `CFIN:LTE`.

### 2.3 Basic — Server Tab

Shows only the minimal MQTT fields (or basic HTTP/CoAP). Full configuration is in Advanced Mode.

```
┌─ Server Type ─────────────────────────────────────────────┐
│  Type: [MQTT ▼]                                           │
└──────────────────────────────────────────────────────────-┘

── shown when MQTT ──
┌─ MQTT Settings ───────────────────────────────────────────┐
│  Broker:       [mqtt.thingsboard.cloud______________]     │
│  Device Token: [***********************] [☐ Show]        │
│  💡 Get token from ThingsBoard dashboard                   │
└──────────────────────────────────────────────────────────-┘

── shown when HTTP/HTTPS ──
┌─ HTTP / HTTPS Settings ───────────────────────────────────┐
│  Server URL:   [http://server:8080/api/v1/{token}/...___] │
│  Auth Token:   [_________________________________________] │
│  ☐ Use TLS (HTTPS)                                        │
│  💡 Use {token} in URL to inject auth token               │
└──────────────────────────────────────────────────────────-┘

── shown when CoAP ──
┌─ CoAP Settings ───────────────────────────────────────────┐
│  Host:          [demo.thingsboard.io________________]     │
│  Resource Path: [/api/v1/{token}/telemetry__________]     │
│  Device Token:  [_____________________________________]    │
│  💡 Use {token} in Resource Path to inject device token   │
└──────────────────────────────────────────────────────────-┘

                                         [Set Server Config ✅]
```

| Server Type | Value sent |
|---|---|
| MQTT | `CFSV:0` + `CFMQ:broker\|token\|sub\|pub\|attr` |
| CoAP | `CFSV:1` + `CFCP:host\|path\|token\|port\|dtls\|ack\|rtx` |
| HTTP/HTTPS | `CFSV:2` + `CFHP:url\|token\|port\|tls\|verify\|ms` |

### 2.4 Basic — Interfaces Tab

Read-only informational tab. Shows which module stacks were detected.

```
┌─ Detected LAN Stacks ─────────────────────────────────────┐
│  Stack 1:  002 – STM32WB_BLE_Gateway  (from Read Config) │
│  Stack 2:  003 – RAK3172              (from Read Config) │
│                                                           │
│  💡 Module config tabs appear automatically when a        │
│     module stack is detected.                             │
└──────────────────────────────────────────────────────────-┘
```

### 2.5 Basic — Dynamic Module Tabs (BLE only in current code)

When a BLE stack is detected (stack_id = `002` or `004`), a tab `🔷 BLE Stack 1` is added dynamically. Future: Zigbee/LoRa basic tabs.

```
┌─ 🔷 BLE Stack 1  (STM32WB_BLE_Gateway) ───────────────────────────────────┐
│                                                                            │
│  ┌─ 📤 JSON Config ─────────────────────────────────────────────────────┐ │
│  │  No JSON loaded                                                       │ │
│  │  [📋 Default]  [📂 Custom]  [📤 Send]                               │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌─ ⚡ Quick Controls ───────────────────────────────────────────────────┐ │
│  │  [SW Reset]  [Get Info]  [Get Status]  [Scan]  [Stop Scan]           │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌─ 🔗 Connection ───────────────────────────────────────────────────────┐ │
│  │  Connect:     [MAC or address___________]  [Send]                    │ │
│  │  Disconnect:  [MAC or address___________]  [Send]                    │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌─ 📋 Response ─────────────────────────────────────────────────────────┐ │
│  │  (scrollable response text, color-coded)                              │ │
│  │  ← AT+RESP:OK                                                        │ │
│  │                                                    [🗑 Clear]         │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────────────┘
```

**JSON Config panel:**
- `📋 Default` → loads the default JSON for this stack ID from bundled config files
- `📂 Custom` → file picker for custom JSON
- `📤 Send` → sends `CFML:CFBL:JSON:0:{...json...}` (disabled until JSON loaded)
- Shows metadata: filename, function count, byte size

**Quick Controls:** Each button sends `CFML:CFBL:0:{COMMAND_STRING}` where command string comes from `stack_00X_app_commands.json`

**Connection:** Entry + Send pattern. Sends `CFML:CFBL:0:{command_prefix}{user_input}`

**Response area:**
- Green text: OK/success patterns
- Red text: FAIL/ERROR patterns
- Blue text: sent commands (`→ cmd`)
- `🗑 Clear` button

---

## 3. ADVANCED MODE Panel

The Advanced Panel has **7 fixed tabs**:

```
┌─ ⚙️ ADVANCED CONFIGURATION ───────────────────────────────────────────────┐
│  [📶 WiFi]  [📱 LTE]  [☁️ Server]  [🔷 BLE]  [🟩 LoRa]  [🔶 Zigbee]  [🔄 FW]
│                                                                            │
│  (tab content — see sections below)                                       │
└───────────────────────────────────────────────────────────────────────────┘
```

---

### 3.1 Advanced — WiFi Tab

```
┌─ WiFi Network Settings ───────────────────────────────────┐
│  SSID:       [_________________________________]           │
│  Password:   [***********************] [☐ Show]          │
└──────────────────────────────────────────────────────────-┘

┌─ Authentication ──────────────────────────────────────────┐
│  Auth Mode:  [PERSONAL ▼]                                 │
│  Username:   [_________________________________]  ← only ENTERPRISE
└──────────────────────────────────────────────────────────-┘

                                       [✅ Set WiFi Config]

──────────────────────────────────────────────────────────
Commands: CFWF:SSID:PASSWORD:AUTH_MODE
          CFWF:SSID:PASSWORD:USERNAME:AUTH_MODE
Then CFIN:WIFI (after 1s) | AUTH_MODE: PERSONAL or ENTERPRISE
```

**Same fields as Basic WiFi tab** but with separate LabelFrame grouping for network/auth sections, and a command hint footer.

---

### 3.2 Advanced — LTE Tab

```
┌─ Modem Identity ──────────────────────────────────────────┐
│  WAN Stack ID: 100  (ESP32-S3 WAN)   ← read-only, from config
│  Modem Name:  [A7600C1__________________]                  │
│  Comm Type:   [USB ▼]                                      │
└──────────────────────────────────────────────────────────-┘

┌─ APN Settings ────────────────────────────────────────────┐
│  APN:        [m3-world___________________]               │
│  Username:   [_________________________________]           │
│  Password:   [***********************] [☐ Show]          │
└──────────────────────────────────────────────────────────-┘

┌─ Connection Settings ─────────────────────────────────────┐
│  Auto Reconnect:   [☑ Enable]                             │
│  Reconnect Timeout: [30000____] ms  (5000–300000)         │
│  Max Retry:         [0________]    (0 = unlimited)        │
└──────────────────────────────────────────────────────────-┘

┌─ Module GPIO Pins ────────────────────────────────────────┐
│  PWR Pin: [WK ▼]   WK=WAKE#, PE=PERST#, 01–11=GPIO       │
│  RST Pin: [PE ▼]                                          │
└──────────────────────────────────────────────────────────-┘

                                          [✅ Set LTE Config]

──────────────────────────────────────────────────────────
Cmd: CFLT:MODEM:APN:USER:PASS:COMM:AUTO:TIMEOUT_MS:MAX_RETRY:PWR_PIN:RST_PIN
Example: CFLT:A7600C1:m3-world:::USB:true:30000:0:WK:PE
```

| Field | Widget | Values / Default |
|---|---|---|
| WAN Stack ID | Read-only label (blue, bold) | from `config.wan.stack_wan_id` |
| Adapter info | Read-only label (grey) | from `stack_id_map.json` |
| Modem Name | Text input | `"A7600C1"` |
| Comm Type | Select | `USB` / `UART`, default `USB` |
| APN | Text input | `""` |
| Username | Text input | `""` |
| Password | Password input + Show toggle | `""` |
| Auto Reconnect | Checkbox | `true` |
| Reconnect Timeout | Number spinner (5000–300000, step 5000) | `30000` |
| Max Retry | Number spinner (0–100) | `0` |
| PWR Pin | Select | `WK`, `PE`, `01`–`11`, default `WK` |
| RST Pin | Select | same options, default `PE` |

---

### 3.3 Advanced — Server Tab

The most complex WAN tab. Server type dropdown controls which sub-section is visible.

```
┌─ Server Type ─────────────────────────────────────────────┐
│  Type: [MQTT ▼]                                           │
└──────────────────────────────────────────────────────────-┘
```

**When MQTT selected:**
```
┌─ MQTT Settings ───────────────────────────────────────────┐
│  Broker URI:    [mqtt://demo.thingsboard.io:1883_______]  │
│  Device Token:  [_________________________________________] │
│  ℹ️  Format: mqtt[s]://host:port                          │
└──────────────────────────────────────────────────────────-┘

┌─ MQTT Topics ─────────────────────────────────────────────┐
│  Subscribe Topic:  [v1/devices/me/rpc/request/+_______]   │
│  Publish Topic:    [v1/devices/me/telemetry___________]   │
│  Attribute Topic:  [v1/devices/me/attributes__________]   │
└──────────────────────────────────────────────────────────-┘
```

**When HTTP/HTTPS selected:** (MQTT and Topics sections hidden)
```
┌─ HTTP / HTTPS Settings ───────────────────────────────────┐
│  Server URL:    [http://demo.thingsboard.io:8080/api/...] │
│  Auth Token:    [_________________________________________] │
│  Port:          [8080____]   Timeout (ms): [10000_____]   │
│  ☐ Use TLS (HTTPS)   ☐ Verify Server Cert               │
│  ℹ️  Use {token} in URL to inject the auth token          │
└──────────────────────────────────────────────────────────-┘
```

**When CoAP selected:** (MQTT, Topics, HTTP sections hidden)
```
┌─ CoAP Settings ───────────────────────────────────────────┐
│  Host:           [demo.thingsboard.io___________________]  │
│  Resource Path:  [/api/v1/{token}/telemetry_____________]  │
│  Device Token:   [_________________________________________]│
│  Port:  [5683___]  ACK Timeout (ms): [2000___] Max Retx: [4]
│  ☐ Use DTLS (CoAPS — port 5684)                          │
│  ℹ️  Use {token} in Resource Path to inject the device token
└──────────────────────────────────────────────────────────-┘
```

**Bottom (always visible):**
```
                                     [✅ Set Server Config]

──────────────────────────────────────────────────────────
MQTT: CFSV:0 + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR
HTTP: CFSV:2 + CFHP:URL|TOKEN|PORT|TLS|VERIFY|TIMEOUT
CoAP: CFSV:1 + CFCP:HOST|PATH|TOKEN|PORT|DTLS|ACK_TO|MAX_RTX
```

---

### 3.4 Advanced — BLE Tab

Layout: **Header** (Stack slot + Preset + Module ID/Name) + **Two-pane body** (Left = scrollable form, Right = JSON preview + Actions + Status)

```
┌─ 🔷 BLE ──────────────────────────────────────────────────────────────────┐
│  Stack Slot: [S1 ▼]   Preset: [BLE (STM32WB55) ▼]   [🔄 Reload]          │
│  Module ID:  [002___________]  Module Name: [STM32WB_BLE_Gateway________] │
│  ─────────────────────────────────────────────────────────────────────── │
│  LEFT (scrollable ~55%)                │  RIGHT (~45%)                    │
│                                        │                                  │
│  ┌─ 🔌 Communication ──────────────┐  │  ┌─ 📄 Generated JSON ─────────┐ │
│  │  (Section 3.4.1)                │  │  │  (Section 3.4.3)            │ │
│  └─────────────────────────────────┘  │  └────────────────────────────-┘ │
│                                        │                                  │
│  ┌─ ⚙️ Functions ───────────────────┐  │  ┌─ 🚀 Actions ────────────────┐ │
│  │  (Section 3.4.2)                │  │  │  (Section 3.4.4)            │ │
│  └─────────────────────────────────┘  │  └────────────────────────────-┘ │
│                                        │                                  │
│                                        │  ┌─ 📊 Status ─────────────────┐ │
│                                        │  │  (Section 3.4.5)            │ │
│                                        │  └────────────────────────────-┘ │
└────────────────────────────────────────────────────────────────────────────┘
```

**Header widgets:**

| Widget | Type | Options | Notes |
|---|---|---|---|
| Stack Slot | Select (readonly) | `S1`, `S2` | Which slot on gateway |
| Preset | Select (editable) | `BLE (STM32WB55)`, `BLE (Custom)` | Selecting auto-fills Module ID + Name |
| Module ID | Text input | default `002` | Written to JSON `module_id` |
| Module Name | Text input | default `STM32WB_BLE_Gateway` | Written to JSON `module_name` |
| 🔄 Reload | Button | — | Re-loads preset into form |

**Preset → Module ID mapping:**
- `BLE (STM32WB55)` → ID `002`
- `BLE (Custom)` → ID `004`

---

#### 3.4.1 Communication Section (left panel, top)

```
┌─ 🔌 Communication ────────────────────────────────────────┐
│  Port type: [uart ▼]                                      │
│                                                           │
│  ── visible when uart or usb ──                          │
│  Baudrate:  [115200 ▼]                                   │
│                                                           │
│  ── visible only when uart ──                            │
│  Parity:    [none ▼]                                     │
│  Stop bit:  [1 ▼]                                        │
│                                                           │
│  ── visible only when spi ──                             │
│  SPI mode:  [0 ▼]                                        │
│  Clock Hz:  [1000000    ]  (spinner 100000–40000000)     │
│  CS pin:    [05_________]                                │
│                                                           │
│  ── visible only when i2c ──                             │
│  I2C addr:  [0x60_______]                                │
│  Clock Hz:  [400000     ]  (spinner 10000–1000000)       │
└──────────────────────────────────────────────────────────-┘
```

Changing Port type hides/shows the corresponding sub-fields. All changes update the JSON preview in real-time.

| Field | Widget | Options | Default |
|---|---|---|---|
| Port type | Select | `uart`, `usb`, `spi`, `i2c` | `uart` |
| Baudrate | Select | `9600`, `38400`, `57600`, `115200`, `230400` | `115200` |
| Parity | Select | `none`, `odd`, `even` | `none` |
| Stop bit | Select | `1`, `2` | `1` |
| SPI mode | Select | `0`, `1`, `2`, `3` | `0` |
| SPI Clock Hz | Number spinner | 100000–40000000 | `1000000` |
| SPI CS pin | Text input | — | `05` |
| I2C addr | Text input (hex) | — | `0x60` |
| I2C Clock Hz | Number spinner | 10000–1000000 | `400000` |

---

#### 3.4.2 Functions Section (left panel, below Communication)

Accordion-style grouped list. Each group can be collapsed/expanded. Each function item can expand to show its detail fields.

```
┌─ ⚙️ Functions ─────────────────────────────────────────────┐
│                                                            │
│  ▼ 🔄 System  (7 functions)        ← click to collapse/expand group
│  │                                                        │
│  │  ▸ MODULE_HW_RESET        ☑ Enabled  ← arrow = expand item
│  │  ▾ MODULE_SW_RESET        ☑ Enabled  ← expanded item
│  │  │  Command:       [AT+RST____________________________] │
│  │  │  Is Prefix:     [☐]                                │
│  │  │  GPIO Start:    [01 LOW ✕]  [+ Add GPIO]           │
│  │  │  Delay Start:   [0________] ms                     │
│  │  │  Expect Resp:   [OK_______________________________]  │
│  │  │  Timeout:       [1000_____] ms                     │
│  │  │  GPIO End:      [+ Add GPIO]                       │
│  │  │  Delay End:     [0________] ms                     │
│  │  ▸ MODULE_FACTORY_RESET   ☑ Enabled                   │
│  │  ▸ MODULE_ENTER_CMD_MODE  ☑ Enabled                   │
│  │  ...                                                   │
│                                                            │
│  ▼ ℹ️ Info  (3 functions)                                   │
│  │  ▸ MODULE_GET_INFO        ☑ Enabled                   │
│  │  ▸ MODULE_GET_CONNECTION_STATUS  ☑ Enabled            │
│  │  ▸ MODULE_GET_DIAGNOSTICS ☑ Enabled                   │
│                                                            │
│  ▼ ⚙️ Config  (3 functions)                                 │
│  │  ▸ MODULE_SET_NAME        ☑ Enabled                   │
│  │  ▸ MODULE_SET_COMM_CONFIG ☑ Enabled                   │
│  │  ▸ MODULE_SET_RF_PARAMS   ☑ Enabled                   │
│                                                            │
│  ▼ 🔍 Discovery  (3 functions)                              │
│  │  ▸ MODULE_START_DISCOVERY ☑ Enabled                   │
│  │  ...                                                   │
│                                                            │
│  ▼ 🔗 Connection  (3 functions)                             │
│  │  ▸ MODULE_CONNECT         ☑ Enabled                   │
│  │  ...                                                   │
│                                                            │
│  ▼ 📨 Data  (1 function)                                    │
│  │  ▸ MODULE_SEND_DATA       ☑ Enabled                   │
└──────────────────────────────────────────────────────────-┘
```

**BLE Function Groups (from `config_form.py` FUNCTION_GROUPS):**

| Group | Emoji | Functions |
|---|---|---|
| System | 🔄 | `MODULE_HW_RESET`, `MODULE_SW_RESET`, `MODULE_FACTORY_RESET`, `MODULE_ENTER_CMD_MODE`, `MODULE_ENTER_SLEEP`, `MODULE_WAKEUP`, `MODULE_START_BROADCAST` |
| Info | ℹ️ | `MODULE_GET_INFO`, `MODULE_GET_CONNECTION_STATUS`, `MODULE_GET_DIAGNOSTICS` |
| Config | ⚙️ | `MODULE_SET_NAME`, `MODULE_SET_COMM_CONFIG`, `MODULE_SET_RF_PARAMS` |
| Discovery | 🔍 | `MODULE_START_DISCOVERY`, `MODULE_DISCOVER_SERVICES`, `MODULE_DISCOVER_CHARACTERISTICS` |
| Connection | 🔗 | `MODULE_CONNECT`, `MODULE_DISCONNECT`, `MODULE_ENTER_DATA_MODE` |
| Data | 📨 | `MODULE_SEND_DATA` |

**Per-function expanded fields (standard — BLE and LoRa):**

| Field | Widget | Notes |
|---|---|---|
| Command | Text input (wide) | AT command string |
| Is Prefix | Checkbox | When checked, command is used as prefix + param |
| GPIO Start | Dynamic list (Pin select + State select + Remove btn) | GPIOs to set before sending command |
| Delay Start | Spinner (ms, step 50) | Wait after GPIO start before sending |
| Expect Resp | Text input | Expected response substring |
| Timeout | Spinner (ms, step 100) | Command timeout |
| GPIO End | Dynamic list | GPIOs to set after response received |
| Delay End | Spinner (ms, step 50) | Wait after GPIO end |

**Per-function expanded fields (Zigbee only — adds):**

| Field | Widget | Notes |
|---|---|---|
| CMD Type | Text input (hex, e.g. `0x01`) | Shows dec value in parentheses |
| CMD Code | Text input (hex) | Shows dec value parentheses |
| Resp Format | Select: `ascii` / `hex` | — |
| ⚡ async badge | Visual badge only | Shown when `is_async_event = true`; disables Timeout |

**GPIO Start / GPIO End row format:**
```
Pin: [01 ▼]  State: [LOW ▼]  [✕]     ← existing row
                              [+ Add GPIO]  ← add button
```

Pin options: `01`, `02`, ..., `11`, `WK`, `PE`  
State options: `LOW`, `HIGH`

---

#### 3.4.3 JSON Preview (right panel, top)

```
┌─ 📄 Generated JSON ────────────────────────────────────────┐
│  ┌──────────────────────────────────────────────────────┐  │
│  │ {                                                    │  │
│  │   "module_id": "002",                               │  │
│  │   "module_type": "BLE",                             │  │
│  │   "module_name": "STM32WB_BLE_Gateway",             │  │
│  │   "module_communication": {                         │  │
│  │     "port_type": "uart",                            │  │
│  │     "parameters": {                                 │  │
│  │       "baudrate": 115200,                           │  │
│  │       "parity": "none",                             │  │
│  │       "stopbit": 1                                  │  │
│  │     }                                               │  │
│  │   },                                                │  │
│  │   "functions": [...]                                │  │
│  │ }                                                   │  │
│  └──────────────────────────────────────────────────┘  │  │
│  (scrollable — editable — realtime update)              │  │
└────────────────────────────────────────────────────────────┘
```

- **Realtime update**: any form change instantly regenerates the JSON
- **Editable textarea**: user can type directly into the JSON — form syncs back
- **Font**: monospace (Consolas) 9pt
- **Background**: `#FAFAFA`

---

#### 3.4.4 Actions Panel (right panel, middle)

```
┌─ 🚀 Actions ───────────────────────────────────────────────┐
│  [📋 Generate]  [💾 Save JSON]  [📂 Load JSON]             │
│  [📤 Send JSON Config]                                      │
│  [⚡ Quick CMD: Send Function ▼]  [▶ Run]                  │
└────────────────────────────────────────────────────────────┘
```

| Button | Action |
|---|---|
| `📋 Generate` | Re-builds JSON from all form fields into preview |
| `💾 Save JSON` | Downloads current JSON to file |
| `📂 Load JSON` | Opens file picker → loads JSON into preview and form |
| `📤 Send JSON Config` | `POST /api/config` with `{ "ble_json": "<JSON>" }` for slot S1/S2 → sends `CFML:CFBL:JSON:{...}` |
| Quick CMD dropdown | Lists all enabled `function_name` values | 
| `▶ Run` | Sends selected function as AT command: `POST /api/config` with `{ "ble_cmd": { "slot": "S1", "function": "MODULE_HW_RESET" } }` |

---

#### 3.4.5 Status Panel (right panel, bottom)

```
┌─ 📊 Status ────────────────────────────────────────────────┐
│  Last action:  ✅ JSON config sent (1248 bytes)            │
│  Response:     CFBL:JSON:OK                                │
│  Timestamp:    14:32:01                                    │
└────────────────────────────────────────────────────────────┘
```

Displays the last command result (success/fail) with timestamp.

---

### 3.5 Advanced — LoRa Tab

Identical layout structure to BLE Tab (Header + Two-pane: Communication + Functions | JSON Preview + Actions + Status).

**Preset options:**
- `LoRa (RAK3172)` → Module ID `003`
- `LoRa (Wio-E5 mini)` → Module ID `006`

**Sends:** `CFML:CFLR:JSON:{...}` or `CFML:CFLR:S1:FUNCTION_NAME`

**LoRa Function Groups:**

| Group | Emoji | Functions |
|---|---|---|
| System | 🔄 | `MODULE_HW_RESET`, `MODULE_SW_RESET`, `MODULE_GET_INFO`, `MODULE_FACTORY_RESET` |
| Region & Class | 🌍 | `MODULE_SET_REGION`, `MODULE_SET_CLASS` |
| OTAA Provisioning | 🔑 | `MODULE_SET_JOIN_MODE`, `MODULE_SET_DEVEUI`, `MODULE_GET_DEVEUI`, `MODULE_SET_APPEUI`, `MODULE_SET_APPKEY`, `MODULE_JOIN`, `MODULE_GET_JOIN_STATUS` |
| ABP Provisioning | 🔒 | `MODULE_SET_DEVADDR`, `MODULE_SET_NWKSKEY`, `MODULE_SET_APPSKEY` |
| MAC & RF Settings | 📶 | `MODULE_SET_DR`, `MODULE_SET_ADR`, `MODULE_SET_TXP`, `MODULE_SET_CHANNEL`, `MODULE_SET_CONFIRM`, `MODULE_SET_PUBLIC_NET` |
| Data | 📨 | `MODULE_SEND_UNCONFIRMED`, `MODULE_SEND_CONFIRMED`, `MODULE_READ_RECV` |

---

### 3.6 Advanced — Zigbee Tab

Identical layout structure to BLE Tab.

**Preset options:**
- `Zigbee (E180-ZG120B)` → Module ID `001`
- `Zigbee (STM32WB55)` → Module ID `005`

**Sends:** `CFML:CFZB:JSON:{...}` or `CFML:CFZB:S1:FUNCTION_NAME`

**Zigbee Function Groups:**

| Group | Emoji | Functions |
|---|---|---|
| Lifecycle | 🔄 | `MODULE_HW_RESET`, `MODULE_SW_RESET`, `MODULE_FACTORY_RESET`, `MODULE_GET_INFO`, `MODULE_ENTER_HEX_MODE` |
| Network Management | 🌐 | `MODULE_START_NETWORK`, `MODULE_STOP_NETWORK`, `MODULE_GET_NET_STATUS`, `MODULE_SET_CHANNEL`, `MODULE_SET_PANID`, `MODULE_SET_TX_POWER`, `MODULE_SET_PERMIT_JOIN` |
| Node Discovery | 🔍 | `MODULE_NODE_JOIN_NOTIFY`, `MODULE_NODE_LEAVE_NOTIFY`, `MODULE_NODE_ANNOUNCE_NOTIFY`, `MODULE_QUERY_SHORT_ADDR`, `MODULE_QUERY_NODE_PORT_INFO`, `MODULE_DELETE_NODE` |
| ZCL Control | ⚡ | `MODULE_ZCL_READ_ATTR`, `MODULE_ZCL_WRITE_ATTR`, `MODULE_ZCL_SEND_CONTROL_CMD`, `MODULE_ZCL_RECV_CONTROL_CMD`, `MODULE_ZCL_RECV_ATTR_REPORT`, `MODULE_ZCL_SET_REPORT_RULE` |
| Data Transfer | 📨 | `MODULE_SEND_UNICAST`, `MODULE_SEND_BROADCAST` |

**Zigbee-specific extra fields per function (in addition to standard fields):**
- `CMD Type` (hex input) — with decimal display `(dec: N)`
- `CMD Code` (hex input) — with decimal display
- `Resp Format` (select: `ascii` / `hex`)
- `⚡ async` badge on header row for async event functions (disables Timeout spinner)

---

### 3.7 Advanced — Firmware Tab (🔄 FW)

```
┌─ Information ─────────────────────────────────────────────┐
│  Flash both WAN and LAN MCU firmware                      │
│  ⚠️  WAN: provide OTA URL below. LAN: managed over SPI.   │
└──────────────────────────────────────────────────────────-┘

┌─ WAN OTA Update ──────────────────────────────────────────┐
│  OTA URL: [https://example.com/firmware.bin____________]   │
│  ☐ Force update (ignore version check)                   │
│                                                           │
│                               [🔄 Start WAN OTA Update]   │
└──────────────────────────────────────────────────────────-┘

┌─ LAN OTA Update ──────────────────────────────────────────┐
│  OTA URL: [https://example.com/lan_firmware.bin________]   │
│  ☐ Force update (ignore version check)                   │
│                                                           │
│                               [🔄 Start LAN OTA Update]   │
└──────────────────────────────────────────────────────────-┘

┌─ Flash Log ───────────────────────────────────────────────┐
│  (dark terminal-style log panel, bg #1E1E1E, fg #CCCCCC)  │
│  [INFO] Starting OTA...                                   │
│  [SUCCESS] OTA completed!                                 │
│                                                           │
└──────────────────────────────────────────────────────────-┘
```

**Notes:**
- Python app: launches local `flash_WAN.bat` script via subprocess
- Web app: sends `POST /api/config` with `{ "wan_fota": "URL" }` (for WAN OTA) or `{ "lan_fota": "URL" }` (for LAN OTA, forwarded as `CFML:CFFW:URL`)
- Flash log polls `GET /api/status` or uses SSE/WebSocket for realtime progress

---

## 4. Global Interaction Patterns

### 4.1 Read Config (`📖 Read Config` button)

1. `GET /api/config`
2. Response JSON parsed
3. All form fields in both Basic and Advanced panels are populated
4. Dynamic tabs (BLE Stack 1, etc.) are added/removed based on `lan.stack1_id`, `lan.stack2_id`
5. Status: "Config loaded"

### 4.2 Set Config (any `✅ Set ... Config` button)

1. Validate form fields (show inline error on invalid field)
2. `POST /api/config` with relevant JSON
3. On success: status "Config set — restarting…" 
4. On failure: show error toast/alert

### 4.3 Send JSON Config (`📤 Send JSON Config`)

1. Minify JSON from preview
2. `POST /api/config` with `{ "ble_json": "...", "slot": "S1" }` (or `lora_json` / `zigbee_json`)
3. Show byte count and result in Status panel

### 4.4 Quick CMD (`▶ Run`)

1. Post `{ "ble_cmd": { "slot": "S1", "function": "MODULE_HW_RESET" } }`
2. Show result in Status panel

### 4.5 Save File / Load File

- **Save**: `JSON.stringify(currentFormState)` → download as `.json` file
- **Load**: file picker → parse JSON → call same function as `Read Config` to populate form

---

## 5. Form Validation Rules

| Field | Rule |
|---|---|
| WiFi SSID | Non-empty |
| WiFi Username (Enterprise) | Non-empty when auth = ENTERPRISE |
| LTE APN | Non-empty |
| LTE Modem Name | Non-empty |
| MQTT Broker URI | Non-empty |
| HTTP Server URL | Non-empty |
| Firmware OTA URL | Must start with `http://` or `https://` |
| Spinner values | Must be within min/max bounds |
| Hex input (I2C addr, CMD Type, CMD Code) | Must be valid hex (`0x...`) or `N/A` |

Show inline error below the invalid field (red text). Prevent form submission if any required field is empty.

---

## 6. Notification / Feedback System

| Event | Python App | Web App |
|---|---|---|
| Command sent | Log entry green `✓ WiFi Config set` | Toast notification (green, 3s auto-dismiss) |
| Command failed | Log entry red `✗ Send failed` | Toast notification (red, persistent) |
| Config loaded | Log entry blue `Config loaded` | "Config loaded ✓" status text |
| Not connected | `messagebox.showwarning` popup | Inline error banner / toast |
| Validation error | `messagebox.showwarning` popup | Inline field error + red border |

---

## 7. Responsive Layout Notes (Web-specific)

| Screen width | Layout |
|---|---|
| ≥ 1200px | Two-pane layout for BLE/LoRa/Zigbee (Left + Right panels side by side) |
| 900px–1200px | Two-pane with reduced widths |
| < 900px | Single column: Communication and Functions on top, JSON Preview below |

The overall app container should be a maximum `1300px` wide, centered, matching the Python app's `"1300x800"` window size.

---

## 8. Tab Navigation Summary

### Basic Mode Tabs
| Tab | Icon | Always shown? | Content |
|---|---|---|---|
| WiFi | 📶 | Yes | SSID, Password, Auth, Username |
| LTE | 📱 | Conditional (if LTE adapter detected) | APN, User, Pass |
| Server | 🌐 | Yes | Type selector + MQTT/HTTP/CoAP minimal fields |
| Interfaces | 🔌 | Yes | Read-only detected stacks info |
| BLE Stack N | 🔷 | Per detected stack (IDs 002,004) | JSON + Quick Controls + Connection + Response |

### Advanced Mode Tabs
| Tab | Icon | Always shown? | Content |
|---|---|---|---|
| WiFi | 📶 | Yes | Full WiFi settings |
| LTE | 📱 | Yes | Full LTE settings (all 11 fields) |
| Server | ☁️ | Yes | MQTT+Topics / HTTP / CoAP full config |
| BLE | 🔷 | Yes | Full JSON Config Builder |
| LoRa | 🟩 | Yes | Full JSON Config Builder |
| Zigbee | 🔶 | Yes | Full JSON Config Builder (+ Zigbee extras) |
| Firmware | 🔄 | Yes | OTA URL inputs + flash log |

---

## 9. RS485 Tab (not in main notebook — separate from Advanced tabs)

The Python app has a `rs485_tab.py` which is instantiated but **not added to the advanced notebook** in the current code (it exists as a file but `advanced_panel.py` does not `notebook.add()` it). 

For the web app, RS485 configuration can be placed as a sub-section inside the **Interfaces** tab in Basic mode, or as a separate expansion panel anywhere.

```
┌─ RS485 Settings ──────────────────────────────────────────┐
│  Baud Rate: [115200 ▼]                                    │
│  Data format: 8N1  (fixed)                                │
│                                                           │
│                                         [Set RS485 Config] │
└──────────────────────────────────────────────────────────-┘
```

Sends: `CFML:CFRS:BR:115200`

Valid baud rates: `9600`, `19200`, `38400`, `57600`, `115200`

---

## 10. JSON Config Structure Reference

The JSON generated by BLE/LoRa/Zigbee form is structured as:

```json
{
  "module_id": "002",
  "module_type": "BLE",
  "module_name": "STM32WB_BLE_Gateway",
  "module_communication": {
    "port_type": "uart",
    "parameters": {
      "baudrate": 115200,
      "parity": "none",
      "stopbit": 1
    }
  },
  "functions": [
    {
      "function_name": "MODULE_HW_RESET",
      "command": "AT+RST",
      "is_prefix": false,
      "gpio_start_control": [
        { "pin": "01", "state": "LOW" }
      ],
      "delay_start": 100,
      "expect_response": "OK",
      "timeout": 1000,
      "gpio_end_control": [],
      "delay_end": 0
    },
    ...
  ]
}
```

For SPI port type:
```json
"module_communication": {
  "port_type": "spi",
  "parameters": {
    "spi_mode": 0,
    "clock_hz": 1000000,
    "cs_pin": "05"
  }
}
```

For I2C port type:
```json
"module_communication": {
  "port_type": "i2c",
  "parameters": {
    "address": "0x60",
    "clock_hz": 400000
  }
}
```

Zigbee function additional fields:
```json
{
  "function_name": "MODULE_ZCL_SEND_CONTROL_CMD",
  "command": "",
  "is_prefix": false,
  "cmd_type": 1,
  "cmd_code": 2,
  "response_format": "hex",
  "is_async_event": false,
  ...
}
```
