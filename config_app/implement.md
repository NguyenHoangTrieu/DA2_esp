# Implementation Plan: ESP32 Gateway Configuration Tool v4.0

## 📋 Tổng quan

Tài liệu này mô tả chi tiết kế hoạch triển khai lại ứng dụng cấu hình Gateway ESP32 với giao diện người dùng thân thiện, dễ sử dụng cho cả người dùng không chuyên về kỹ thuật.

---

## 🎯 Mục tiêu thiết kế

1. **Đơn giản hóa** - Chia thành 2 mode: BASIC và ADVANCED
2. **Trực quan** - Sử dụng icons, màu sắc, và layout rõ ràng
3. **Guided experience** - Hướng dẫn từng bước cho người dùng
4. **Professional** - Giao diện hiện đại, chuyên nghiệp

---

## 🖼️ Thiết kế UI tổng quan

### Layout chính (3-Panel Layout: Configuration + UART Log + Debug Log)

```
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│  🔌 ESP32 Gateway Configuration Tool                                              [_][□][X] │
├─────────────────────────────────────────────────────────────────────────────────────────────┤
│  Port: [COM3 ▼] [Refresh]  Baud: [115200 ▼]  [🔌 Connect]              ● Connected         │
├─────────────────────────────────────────────────────────────────────────────────────────────┤
│  ☐ Advanced Mode    [📖 Read Config] [💾 Save File] [📂 Load File]                         │
├────────────────────────────────────────────────────┬────────────────────────────────────────┤
│                                                    │                                        │
│  ┌─ CONFIGURATION ─────────────────────────────┐   │  ┌─ UART LOG (COM3) ──────────────┐   │
│  │                                             │   │  │  ☐ Auto-scroll           [Clear]│   │
│  │  [Tab Navigation]                           │   │  │                                │   │
│  │  ─────────────────────────────────────────  │   │  │  CFLT:internet:::TCP:false:... │   │
│  │                                             │   │  │  OK                            │   │
│  │        << TAB CONTENT >>                    │   │  │  CFIN:LTE                      │   │
│  │                                             │   │  │  OK                            │   │
│  │  Mỗi tab có nút [Set] riêng                 │   │  │  ...                           │   │
│  │                                             │   │  │                                │   │
│  │  ─────────────────────────────────────────  │   │  │  (Raw UART data từ Gateway)    │   │
│  │                                             │   │  │                                │   │
│  │  Không có khoảng trống thừa                 │   │  │                                │   │
│  │  Content co giãn theo nội dung              │   │  │                                │   │
│  │                                             │   │  │                                │   │
│  └─────────────────────────────────────────────┘   │  └────────────────────────────────┘   │
├────────────────────────────────────────────────────┴────────────────────────────────────────┤
│  ┌─ DEBUG LOG ──────────────────────────────────────────────────────────────────────────┐  │
│  │  ☐ Auto-scroll                                                                [Clear]│  │
│  │  [20:31:38] ✓ Connected to COM40 at 115200 baud                                     │  │
│  │  [20:31:39] ℹ️ Sending: LTE Config                                                   │  │
│  │  [20:31:41] ⚠️ LTE Config - Response: CFLT:internet:::TCP:false:30000:0              │  │
│  │  [20:31:41] ℹ️ Sending: Set Internet Type = LTE                                      │  │
│  │  [20:31:44] ⚠️ Set Internet Type = LTE - Response: CFIN:LTE                          │  │
│  └──────────────────────────────────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────────────────────────────────┤
│  Ready                                                              v4.0 | © 2024          │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Nguyên tắc Layout quan trọng

| Vấn đề | Giải pháp |
|--------|-----------|
| 2 nút BASIC/ADVANCED to đùng | Thay bằng **checkbox** `☐ Advanced Mode` nhỏ gọn |
| Khoảng trống thừa trong CAN/RS485 | **Không expand** các LabelFrame - content fit theo nội dung |
| Cần phân biệt App Log vs UART Log | **3-panel layout** - UART Log bên phải, Debug Log bên dưới |
| Mất thẩm mỹ khi resize | Cột Configuration có **max-width cố định**, UART Log **expand** |

### Mode Selection

```
☐ Advanced Mode     ← Checkbox đơn giản
                      - Unchecked = BASIC MODE (4 tabs: WiFi, LTE, Server, Interfaces)
                      - Checked = ADVANCED MODE (7 tabs: WiFi, LTE, Server, LoRa, CAN, RS485, Firmware)
```

### UART Log Panel (Bên phải - Vùng chính)

| Thuộc tính | Mô tả |
|------------|-------|
| **Vị trí** | Bên phải của Configuration panel |
| **Nội dung** | Raw data nhận/gửi từ serial port (COM) |
| **Header** | `UART LOG (COMx)` - hiển thị tên port đang kết nối |
| **Tính năng** | Auto-scroll checkbox, Clear button |
| **Font** | Consolas, monospace để dễ đọc raw data |
| **Màu nền** | Có thể dùng màu nhạt để phân biệt (optional) |

**Ví dụ nội dung UART Log:**
```
CFLT:internet:::TCP:false:30000:0
OK
CFIN:LTE
OK
CFSC_RESP:START
[GATEWAY_INFO]
model=ESP32S3_IoT_Gateway
...
```

### Debug Log Panel (Bên dưới - Full width)

| Thuộc tính | Mô tả |
|------------|-------|
| **Vị trí** | Dưới cùng, trải dài full width |
| **Nội dung** | Application events (Connected, Sending, Success, Error...) |
| **Header** | `DEBUG LOG` |
| **Tính năng** | Auto-scroll checkbox, Clear button |
| **Format** | `[HH:MM:SS] icon Message` |
| **Chiều cao** | Cố định khoảng 100-150px, có scrollbar |

**Ví dụ nội dung Debug Log:**
```
[20:31:38] ✓ Connected to COM40 at 115200 baud
[20:31:39] ℹ️ Sending: LTE Config
[20:31:39] 🔧 → Sent: CFLT:internet:::TCP:false:30000:0
[20:31:41] ⚠️ LTE Config - Response timeout
[20:31:41] ℹ️ Sending: Set Internet Type = LTE
[20:31:44] ✓ Set Internet Type = LTE - Success
```

### Connection Bar - Chi tiết

| Thành phần | Mô tả | Format |
|------------|-------|--------|
| Port Dropdown | Hiển thị chỉ tên COM | `COM3`, `COM10` (không có description) |
| Refresh Button | Nút text để refresh danh sách port | `[Refresh]` |
| Baud Dropdown | Chọn baud rate | `9600`, `115200`, etc. |
| Connect Button | Kết nối/ngắt kết nối | `🔌 Connect` / `⏏️ Disconnect` |
| Status | Trạng thái kết nối | `● Connected` / `● Disconnected` |

### Xử lý Layout khi Resize

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  CONFIGURATION (max-width: 550px)    │  UART LOG (expand to fill)          │
│  ────────────────────────────────    │  ──────────────────────────────     │
│  • LabelFrame KHÔNG stretch          │  • Text widget expand cả 2 chiều    │
│  • Content wrap theo nội dung        │  • Scrollbar vertical               │
│  • Scrollbar nếu cần                 │  • Hiển thị raw UART data           │
├──────────────────────────────────────┴─────────────────────────────────────┤
│  DEBUG LOG (full width, fixed height ~120px)                               │
│  ──────────────────────────────────────────────────────────────────────── │
│  • Application events với timestamp                                        │
│  • Color-coded messages (Success=green, Error=red, Info=blue)             │
│  • Scrollbar vertical                                                      │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Phân biệt UART Log vs Debug Log

| Đặc điểm | UART Log | Debug Log |
|----------|----------|-----------|
| **Vị trí** | Bên phải (main area) | Bên dưới (footer area) |
| **Nội dung** | Raw data từ serial port | App events có format |
| **Format** | Plain text, no timestamp | `[HH:MM:SS] icon Message` |
| **Mục đích** | Debug protocol, xem response | Theo dõi flow hoạt động |
| **Khi nào dùng** | Khi cần xem raw UART | Khi cần biết app làm gì |

---

## � Coding Guidelines - Layout Rules

### ⚠️ CRITICAL: Tránh Khoảng Trống Thừa

**Nguyên tắc #1: LabelFrame KHÔNG được expand theo chiều dọc**

```python
# ❌ SAI - sẽ tạo khoảng trống thừa
frame.pack(fill=BOTH, expand=YES)
frame.grid(row=0, sticky="nsew")

# ✅ ĐÚNG - content fit theo nội dung
frame.pack(fill=X, anchor="nw")
frame.grid(row=0, sticky="new")
```

**Nguyên tắc #2: Content Frame có minsize nhưng không maxsize**

```python
# ✅ Configuration panel có width cố định
config_frame.configure(width=550)
config_frame.pack_propagate(False)  # Giữ width cố định

# ✅ Debug Log panel expand
debug_frame.pack(fill=BOTH, expand=YES)
```

**Nguyên tắc #3: Nút [Set] nằm ở cuối mỗi tab, không stretch**

```python
# ✅ Nút Set nằm ở cuối, căn giữa
set_btn = ttk.Button(tab_frame, text="✅ Set Config")
set_btn.pack(pady=10, anchor="center")  # Không fill, không expand
```

**Nguyên tắc #4: PanedWindow cho Split View**

```python
# ✅ Dùng PanedWindow để chia Configuration | Debug Log
paned = ttk.PanedWindow(self, orient=HORIZONTAL)
paned.add(config_frame, weight=0)  # Không expand
paned.add(debug_frame, weight=1)   # Expand to fill
```

---

## �📑 BASIC MODE - Chi tiết

### Mục đích
Dành cho người dùng phổ thông, chỉ cần cấu hình các thông số cơ bản nhất để gateway hoạt động.
**Mỗi tab có nút [Set] riêng** để gửi command tương ứng xuống Gateway - không có nút "Write Config" chung.

### Layout BASIC MODE (Tabbed Interface)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  BASIC CONFIGURATION                                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ Tab Navigation ────────────────────────────────────────────────────┐   │
│  │ [📶 WiFi] [📱 LTE] [🌐 Server] [🔌 Interfaces]                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  ════════════════════════════════════════════════════════════════════════  │
│                                                                             │
│                         << TAB CONTENT HERE >>                              │
│                         (Mỗi tab có nút [Set] riêng)                        │
│                                                                             │
│  ════════════════════════════════════════════════════════════════════════  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Basic Tab 1: WiFi Configuration

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📶 WIFI CONFIGURATION                                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Network Name (SSID):   [MyNetwork________________________] (default: "")  │
│                                                                             │
│   Password:              [********_________________________] [👁]          │
│                          (default: "", min 8 chars for WPA2)               │
│                                                                             │
│   💡 Enter your WiFi credentials                                           │
│                                                                             │
│                              [ ✅ Set WiFi Config ]                         │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────   │
│   Commands: CFIN:WIFI + CFWF:SSID:PASSWORD:PERSONAL                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Basic Tab 2: LTE Configuration

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📱 LTE CONFIGURATION                                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   APN:                   [internet_________________________] (default)     │
│                                                                             │
│   Username:              [_________________________________] (optional)    │
│                                                                             │
│   Password:              [_________________________________] [👁] (opt)    │
│                                                                             │
│   💡 Get APN settings from your carrier                                    │
│                                                                             │
│                              [ ✅ Set LTE Config ]                          │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────   │
│   Commands: CFIN:LTE + CFLT:UART:APN:USER:PASS:false:30000:0               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Basic Tab 3: Server Connection

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🌐 SERVER CONNECTION                                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   MQTT Broker URL:       [mqtt://localhost:1883______________] (default)   │
│                                                                             │
│   Device Token:          [_________________________________] [👁]          │
│                                                                             │
│   💡 Get your device token from ThingsBoard dashboard                      │
│                                                                             │
│                              [ ✅ Set Server Config ]                       │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────   │
│   Commands: CFSV:MQTT + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR                     │
│   (SUB/PUB/ATTR dùng default ThingsBoard topics)                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Basic Tab 4: Communication Interfaces

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🔌 COMMUNICATION INTERFACES                                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Stack 1 (Slot A):      [▼ NONE_________________] (default: NONE)         │
│                          Options: NONE | LORA | RS485 | ZIGBEE | CAN       │
│                                                                             │
│   Stack 2 (Slot B):      [▼ NONE_________________] (default: NONE)         │
│                          Options: NONE | LORA | RS485 | ZIGBEE | CAN       │
│                                                                             │
│   ℹ️ Each stack can connect to different sensor networks                   │
│                                                                             │
│                              [ ✅ Set Interfaces ]                          │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────   │
│   Commands: CFST:ST_1:TYPE + CFST:ST_2:TYPE                                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Các config trong BASIC MODE

**Lưu ý**: Tất cả config ở BASIC MODE đều có mặt trong ADVANCED MODE (với đầy đủ options hơn).

| Tab | Config Fields | Command Format | Default Value |
|-----|--------------|----------------|---------------|
| WiFi | ssid, password | `CFWF:SSID:PASSWORD:PERSONAL` | ssid="", password="" |
| LTE | apn, username, password | `CFLT:UART:APN:USER:PASS:false:30000:0` | apn="internet", user="", pass="" |
| Server | broker, token | `CFMQ:BROKER\|TOKEN\|v1/devices/me/telemetry\|v1/devices/me/rpc/request/+\|v1/devices/me/attributes` | broker="mqtt://localhost:1883" |
| Interfaces | stack_1_type, stack_2_type | `CFST:ST_1:TYPE`, `CFST:ST_2:TYPE` | NONE |

**Actual Commands:**
- **WiFi Tab [Set]**: Gửi `CFIN:WIFI` + `CFWF:ssid:password:PERSONAL`
- **LTE Tab [Set]**: Gửi `CFIN:LTE` + `CFLT:UART:apn:user:pass:false:30000:0`
- **Server Tab [Set]**: Gửi `CFSV:MQTT` + `CFMQ:broker|token|sub|pub|attr`
- **Interfaces Tab [Set]**: Gửi `CFST:ST_1:type` + `CFST:ST_2:type`

---

## ⚙️ ADVANCED MODE - Chi tiết

### Mục đích
Dành cho kỹ thuật viên cần cấu hình chi tiết tất cả các thông số của gateway.

### Layout ADVANCED MODE

**Nguyên tắc quan trọng:**
1. **LabelFrame KHÔNG được stretch** - pack với `anchor="nw"`, không dùng `fill=BOTH expand=YES`
2. **Mỗi tab có nút [Set] riêng** ở cuối để gửi config
3. **Scroll nếu cần** - nếu nội dung dài hơn màn hình, dùng scrollable canvas
4. **Không có khoảng trống thừa** - content fit theo nội dung thực

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ADVANCED CONFIGURATION (width cố định, không expand)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ Tab Navigation ────────────────────────────────────────────────────┐   │
│  │ [📶 WiFi] [📱 LTE] [🌐 Server] [📡 LoRa] [🔌 CAN] [📟 RS485] [🔄 FW]│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  ┌─ Scrollable Content (if needed) ────────────────────────────────────┐   │
│  │                                                                     │   │
│  │      << TAB CONTENT - LabelFrames KHÔNG stretch >>                  │   │
│  │                                                                     │   │
│  │      Các LabelFrame xếp từ trên xuống, không giãn ra               │   │
│  │                                                                     │   │
│  │                    [ ✅ Set Tab Name Config ]                       │   │
│  │                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 1: WiFi Configuration (Advanced)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📶 WIFI CONFIGURATION (ADVANCED)                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ WiFi Network Settings ───────────────────────────────────────────────┐ │
│  │  SSID:           [_______________________________]                    │ │
│  │  Password:       [_______________________________] [👁]               │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ Authentication ──────────────────────────────────────────────────────┐ │
│  │  Auth Mode:      [▼ PERSONAL________]                                 │ │
│  │                  Options: PERSONAL | ENTERPRISE                       │ │
│  │  Username:       [_______________________________] (Enterprise only)  │ │
│  │                  ℹ️ Username chỉ dùng cho Enterprise (WPA2-EAP)       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set WiFi Config ]                         │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────  │
│   Commands: CFIN:WIFI + CFWF:SSID:PASSWORD:AUTH_MODE[:USERNAME]            │
│   AUTH_MODE: 0=PERSONAL, 1=ENTERPRISE                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 2: LTE Configuration (Advanced)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📱 LTE CONFIGURATION (ADVANCED)                                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ APN Settings ────────────────────────────────────────────────────────┐ │
│  │  APN:            [internet_________________________]                  │ │
│  │  Username:       [_______________________________]                    │ │
│  │  Password:       [_______________________________] [👁]               │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ Connection Settings ─────────────────────────────────────────────────┐ │
│  │  Comm Type:      [▼ UART__________]                                   │ │
│  │                  Options: UART | USB                                  │ │
│  │                                                                       │ │
│  │  Auto Reconnect: [✓]                                                  │ │
│  │  Timeout:        [30000_____] ms                                      │ │
│  │  Max Retry:      [5____]         (0 = infinite)                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set LTE Config ]                          │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────  │
│   Commands: CFIN:LTE + CFLT:COMM:APN:USER:PASS:RECONNECT:TIMEOUT:RETRY     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 3: Server Configuration (Advanced)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🌐 SERVER CONFIGURATION (ADVANCED)                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ Server Type ─────────────────────────────────────────────────────────┐ │
│  │  Type:           [▼ MQTT__________]                                   │ │
│  │                  Options: MQTT | HTTP | CoAP                          │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ MQTT Settings ───────────────────────────────────────────────────────┐ │
│  │  Broker URI:     [mqtt.thingsboard.cloud___________]                  │ │
│  │  Device Token:   [_______________________________] [👁]               │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ MQTT Topics ─────────────────────────────────────────────────────────┐ │
│  │  Subscribe:      [v1/devices/me/rpc/request/+_____]                   │ │
│  │  Publish:        [v1/devices/me/telemetry_________]                   │ │
│  │  Attribute:      [v1/devices/me/attributes________]                   │ │
│  │                  ℹ️ Default topics cho ThingsBoard                    │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set Server Config ]                       │
│                                                                             │
│   ────────────────────────────────────────────────────────────────────────  │
│   Commands: CFSV:TYPE + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 4: LoRa Configuration

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📡 LORA CONFIGURATION                                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ LoRa E32 Modem Settings ─────────────────────────────────────────────┐ │
│  │  Address High (ADDH): [0x] [00__]     Address Low (ADDL): [0x] [00__] │ │
│  │  Speed Config (SPED): [▼ 0x1A - 9600bps, 8N1______]                  │ │
│  │    Presets:                                                           │ │
│  │    • 0x1A - 9600bps, 8N1                                             │ │
│  │    • 0x3A - 9600bps, 8N1, Air 2.4kbps                               │ │
│  │    • 0x5A - 9600bps, 8N1, Air 19.2kbps                              │ │
│  │  Channel (0-31):      [▼ 23___]                                      │ │
│  │  Option Config:       [▼ 0x44 - Fixed, Push-Pull______]              │ │
│  │    Presets:                                                           │ │
│  │    • 0x44 - Fixed transmission, Push-Pull, 250ms wakeup             │ │
│  │    • 0x04 - Transparent, Push-Pull, 250ms wakeup                    │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ LoRa TDMA Network Settings ──────────────────────────────────────────┐ │
│  │  Role:            ◉ Gateway    ○ Node                                │ │
│  │  Node ID:         [0x] [0001]       Gateway ID:    [0x] [0001]       │ │
│  │  TDMA Slots:      Total: [8___]     My Slot: [0___]                  │ │
│  │  Slot Duration:   [200__] ms                                          │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ LoRa Encryption ─────────────────────────────────────────────────────┐ │
│  │  Encryption:      [✓] Enable                                         │ │
│  │  Key Length:      [▼ 16 bytes (AES-128)___]                          │ │
│  │  Key:             [________________________________] [🔄 Generate]   │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set LoRa Config ]                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 5: CAN Bus Configuration

**Layout Note:** LabelFrames packed từ trên xuống, KHÔNG expand, KHÔNG fill Y

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🔌 CAN BUS CONFIGURATION                                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ CAN Settings (packed at TOP, no expand) ─────────────────────────────┐ │
│  │  Baud Rate:       [▼ 500000_______________]                          │ │
│  │                   Options: 125000 | 250000 | 500000 | 800000 | 1000000│
│  │  Operating Mode:  [▼ NORMAL_______________]                          │ │
│  │                   Options: NORMAL | LOOPBACK | SILENT                │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ CAN ID Whitelist (packed below, no expand) ──────────────────────────┐ │
│  │  ┌────────────────────────┐     ┌──────────────────────────────────┐ │ │
│  │  │ Allowed CAN IDs:       │     │  Add New ID:                     │ │ │
│  │  │ ────────────────────── │     │  [0x] [___] [➕ Add]              │ │ │
│  │  │ • 0x100               │     │  Quick Add:                      │ │ │
│  │  │ • 0x101               │     │  [Add Range] [Import CSV]        │ │ │
│  │  │ • 0x200               │     └──────────────────────────────────┘ │ │
│  │  └────────────────────────┘     [🗑️ Remove] [🧹 Clear All]          │ │
│  │  Total: 5 IDs                                                        │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set CAN Config ]                          │
│                                                                             │
│  ← Không có khoảng trống thừa bên dưới                                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 6: RS485 Configuration

**Layout Note:** LabelFrame packed từ trên xuống, KHÔNG expand, KHÔNG fill Y

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  📟 RS485 CONFIGURATION                                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ RS485 Serial Settings (packed at TOP, no expand) ────────────────────┐ │
│  │  Baud Rate:       [▼ 115200______________]                           │ │
│  │                   Options: 9600 | 19200 | 38400 | 57600 | 115200     │ │
│  │  ℹ️ Data format is fixed at 8N1 (8 data bits, No parity, 1 stop bit) │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                              [ ✅ Set RS485 Config ]                        │
│                                                                             │
│  ← Không có khoảng trống thừa bên dưới, content fit theo nội dung          │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Tab 7: Firmware Update

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  🔄 FIRMWARE UPDATE                                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─ Thông tin ───────────────────────────────────────────────────────────┐ │
│  │                                                                       │ │
│  │   ℹ️ Firmware update sẽ flash cả WAN và LAN MCU cùng lúc             │ │
│  │                                                                       │ │
│  │   Yêu cầu:                                                            │ │
│  │   • File flash_WAN.bat phải nằm cùng thư mục với ứng dụng            │ │
│  │   • Các file .bin firmware phải được chuẩn bị sẵn                     │ │
│  │   • Chọn đúng COM port kết nối với Gateway                           │ │
│  │                                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌─ COM Port Selection ──────────────────────────────────────────────────┐ │
│  │                                                                       │ │
│  │   Serial Port:    [▼ COM3________________] [🔄 Refresh]              │ │
│  │                                                                       │ │
│  │   ⚠️ Nếu đang kết nối, app sẽ tự động disconnect trước khi flash    │ │
│  │                                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│                    [🔄 Update Firmware (WAN + LAN)]                        │
│                                                                             │
│  ┌─ Flash Output Log ────────────────────────────────────────────────────┐ │
│  │                                                                       │ │
│  │  ══════════════════════════════════════════════════════════════════  │ │
│  │  [INFO] Starting firmware update: BOTH WAN and LAN                   │ │
│  │  [DEBUG] Command: "C:\app\flash_WAN.bat" COM3                        │ │
│  │  [DEBUG] Connecting...                                               │ │
│  │  [DEBUG] Chip is ESP32-S3                                            │ │
│  │  [DEBUG] Erasing flash...                                            │ │
│  │  [DEBUG] Writing at 0x00010000... (10%)                              │ │
│  │  [DEBUG] Writing at 0x00020000... (25%)                              │ │
│  │  [SUCCESS] Hash of data verified.                                    │ │
│  │  [SUCCESS] Firmware update completed!                                │ │
│  │  ══════════════════════════════════════════════════════════════════  │ │
│  │                                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🔄 Firmware Update - Chi tiết chức năng

### Mục đích
Cho phép người dùng cập nhật firmware cho cả WAN MCU và LAN MCU của gateway thông qua file batch script.

### Workflow Update Firmware (Theo code hiện tại)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        FIRMWARE UPDATE WORKFLOW                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌───────────┐     ┌───────────┐     ┌───────────┐     ┌───────────┐     │
│   │  CHECK    │ ──▶ │DISCONNECT │ ──▶ │   RUN     │ ──▶ │  SHOW     │     │
│   │  SCRIPT   │     │  SERIAL   │     │ BATCH CMD │     │  RESULT   │     │
│   └───────────┘     └───────────┘     └───────────┘     └───────────┘     │
│        │                 │                 │                 │             │
│        ▼                 ▼                 ▼                 ▼             │
│   Kiểm tra          Ngắt kết nối      Chạy script      Hiển thị kết       │
│   flash_WAN.bat     serial port       trong thread     quả SUCCESS/       │
│   có tồn tại?       nếu đang kết nối  riêng biệt       ERROR              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Chi tiết Implementation

#### 1. Kiểm tra Script
```python
flash_script = self.app_path / "flash_WAN.bat"
if not flash_script.exists():
    # Hiển thị lỗi nếu không tìm thấy script
    messagebox.showerror("Error", f"flash_WAN.bat not found!")
```

#### 2. Disconnect Serial (nếu đang kết nối)
```python
if self.serial_port and self.serial_port.is_open:
    port = self.serial_port.port
    self.disconnect()
    threading.Event().wait(1)  # Đợi 1 giây
```

#### 3. Chạy Flash Script trong Thread riêng
```python
def flash_thread():
    cmd = f'"{flash_script}" {port}'
    
    process = subprocess.Popen(
        cmd,
        shell=True,
        cwd=str(self.app_path),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding='utf-8',
        errors='ignore',
        bufsize=1
    )
    
    # Đọc output realtime và log ra console
    for line in iter(process.stdout.readline, ''):
        if line:
            line = line.rstrip()
            if 'ERROR' in line.upper() or 'FAILED' in line.upper():
                self.log(line, 'ERROR')    # Màu đỏ
            elif 'SUCCESS' in line.upper() or ' OK' in line:
                self.log(line, 'SUCCESS')  # Màu xanh
            else:
                self.log(line, 'DEBUG')    # Màu mặc định
```

#### 4. Xử lý kết quả
```python
return_code = process.wait()

if return_code == 0:
    self.log("Firmware update completed!", 'SUCCESS')
    messagebox.showinfo("Success", "Firmware updated for both WAN and LAN!")
else:
    self.log(f"Flash failed (code {return_code})", 'ERROR')
    messagebox.showerror("Error", "Firmware update failed")
```

### Log Output Format

| Log Level | Điều kiện | Màu sắc | Ví dụ |
|-----------|-----------|---------|-------|
| `ERROR` | Chứa "ERROR" hoặc "FAILED" | 🔴 Đỏ | `[ERROR] Connection failed` |
| `SUCCESS` | Chứa "SUCCESS" hoặc " OK" | 🟢 Xanh | `[SUCCESS] Hash verified` |
| `DEBUG` | Các dòng còn lại | ⚪ Mặc định | `[DEBUG] Writing at 0x00010000...` |
| `INFO` | Thông báo từ app | 🔵 Xanh dương | `[INFO] Starting firmware update` |

### Cấu trúc thư mục yêu cầu

```
app_directory/
├── gateway_config_tool.exe (hoặc .py)
├── flash_WAN.bat              ← Script flash firmware (BẮT BUỘC)
├── bootloader.bin             ← (nếu cần)
├── partition-table.bin        ← (nếu cần)
└── firmware.bin               ← File firmware
```

### Xử lý lỗi khi Flash

| Lỗi | Mô tả | Xử lý |
|-----|-------|-------|
| `flash_WAN.bat not found` | Không tìm thấy script | Đặt flash_WAN.bat cùng thư mục với app |
| `Connection failed` | Không kết nối được với MCU | Kiểm tra cổng COM, baud rate |
| `Wrong chip` | Chip không đúng loại | Kiểm tra firmware đúng cho WAN/LAN |
| `Verify failed` | Checksum không khớp | Flash lại hoặc kiểm tra file |
| `Timeout` | Quá thời gian chờ | Giảm baud rate, thử lại |
| `return_code != 0` | Script trả về lỗi | Xem chi tiết trong Console Log |

### Tính năng hiện tại

1. **Pre-flash**:
   - Kiểm tra flash_WAN.bat tồn tại
   - Tự động disconnect serial nếu đang kết nối
   - Hiển thị dialog xác nhận trước khi flash

2. **During flash**:
   - Chạy trong thread riêng (không block UI)
   - Real-time log output trong Console
   - Phân loại log theo màu (ERROR/SUCCESS/DEBUG)

3. **Post-flash**:
   - Hiển thị kết quả SUCCESS hoặc ERROR
   - Message box thông báo kết quả

---

## 📋 Bảng tổng hợp tất cả Config

### GATEWAY_INFO (Read-only)

| Config | Type | Description | Example |
|--------|------|-------------|---------||
| `model` | String (Read-only) | Gateway model | `ESP32S3_IoT_Gateway` |
| `firmware` | String (Read-only) | Firmware version | `v1.2.0` |
| `hardware` | String (Read-only) | Hardware version | `HW_v2.0` |
| `serial` | String (Read-only) | Serial number | `GW2025001` |
| `internet_status` | String (Read-only) | Internet connection status | `ONLINE`, `OFFLINE` |
| `rtc_time` | String (Read-only) | Current RTC time | `30/12/2025-14:35:20` |

### WAN Configuration

| Config | Loại Input | Giá trị cho phép | Default ||
|--------|------------|------------------|---------|
| `internet_type` | Dropdown | `WIFI`, `LTE`, `UNKNOWN` | `WIFI` |
| `wifi_ssid` | Text Input | Free text (max 32 chars) | - |
| `wifi_password` | Password Input | Hiển thị `***HIDDEN***` khi có data | - |
| `wifi_username` | Text Input | Free text (Enterprise only) | - |
| `wifi_auth_mode` | Integer | `0` (PERSONAL), `1` (ENTERPRISE) | `0` |
| `lte_apn` | Text Input | Free text | - |
| `lte_username` | Text Input | Free text | - |
| `lte_password` | Password Input | Hiển thị `***HIDDEN***` khi có data | - |
| `lte_comm_type` | Dropdown | `UART`, `USB` | `UART` |
| `lte_max_retries` | Number Input | 0 - 100 (0=infinite) | 5 |
| `lte_timeout_ms` | Number Input | 1000 - 300000 (milliseconds) | 30000 |
| `lte_auto_reconnect` | Dropdown | `true`, `false` | `false` |
| `server_type` | Dropdown | `MQTT`, `HTTP`, `UNKNOWN` | `MQTT` |
| `mqtt_broker` | Text Input | URL format | - |
| `mqtt_device_token` | Password Input | Hiển thị `***HIDDEN***` khi có data | - |
| `mqtt_pub_topic` | Text Input | MQTT topic format | - |
| `mqtt_sub_topic` | Text Input | MQTT topic format | - |
| `mqtt_attribute_topic` | Text Input | MQTT topic format | - |

### LAN Configuration - LoRa

| Config | Loại Input | Giá trị cho phép | Default |
|--------|------------|------------------|---------|
| `lora_e32_addh` | Hex Input | 0x00 - 0xFF | 0x00 |
| `lora_e32_addl` | Hex Input | 0x00 - 0xFF | 0x00 |
| `lora_e32_sped` | Hex Input | 0x00 - 0xFF | 0x1A |
| `lora_e32_chan` | Number Input | 0 - 31 | 23 |
| `lora_e32_option` | Hex Input | 0x00 - 0xFF | 0x44 |
| `lora_e32_baud` | Number (Read-only) | UART baud rate | 9600 |
| `lora_e32_header` | Hex (Read-only) | Header byte | 0xC0 |
| `lora_role` | String | `GATEWAY`, `NODE` | `GATEWAY` |
| `lora_node_id` | Hex Input | 0x0000 - 0xFFFF | 0x0001 |
| `lora_gateway_id` | Hex Input | 0x0000 - 0xFFFF | 0x0001 |
| `lora_num_slots` | Number Input | 1 - 255 | 8 |
| `lora_my_slot` | Number Input | 0 - (num_slots-1) | 0 |
| `lora_slot_duration_ms` | Number Input | 50 - 10000 | 200 |
| `lora_crypto_key_len` | Dropdown | `0` (off), `16`, `32` | 0 |

### LAN Configuration - CAN

| Config | Loại Input | Giá trị cho phép | Default |
|--------|------------|------------------|---------|
| `can_baud_rate` | Dropdown | `125000`, `250000`, `500000`, `800000`, `1000000` | `500000` |
| `can_mode` | Dropdown | `NORMAL`, `LOOPBACK`, `SILENT` | `NORMAL` |
| `can_whitelist_count` | Number (Read-only) | Số lượng CAN IDs trong whitelist | 0 |
| `can_whitelist` | String (Read-only) | Comma-separated CAN IDs | `0x100,0x101,...` |

### LAN Configuration - RS485

| Config | Loại Input | Giá trị cho phép | Default |
|--------|------------|------------------|---------|
| `rs485_baud_rate` | Dropdown | `9600`, `19200`, `38400`, `57600`, `115200` | `115200` |

### LAN Configuration - Stack

| Config | Loại Input | Giá trị cho phép | Default |
|--------|------------|------------------|---------|
| `stack_1_type` | Dropdown | `NONE`, `LORA`, `RS485`, `ZIGBEE`, `CAN` | `NONE` |
| `stack_2_type` | Dropdown | `NONE`, `LORA`, `RS485`, `ZIGBEE`, `CAN` | `NONE` |

---

## 🏗️ Cấu trúc Code

### Tổ chức Files

```
config_app/
├── main.py                    # Entry point
├── requirements.txt           # Dependencies
├── assets/
│   ├── icons/                # Icon files
│   └── styles/               # CSS/Theme files
├── src/
│   ├── __init__.py
│   ├── app.py                # Main Application class
│   ├── config/
│   │   ├── __init__.py
│   │   ├── protocol.py       # Protocol definitions (refactored)
│   │   ├── validators.py     # Input validators
│   │   └── commands.py       # Command builders
│   ├── ui/
│   │   ├── __init__.py
│   │   ├── main_window.py    # Main window layout
│   │   ├── connection_bar.py # Serial connection widget
│   │   ├── mode_selector.py  # Basic/Advanced toggle
│   │   ├── console_panel.py  # Log console widget
│   │   ├── basic/
│   │   │   ├── __init__.py
│   │   │   └── basic_panel.py    # Basic mode UI
│   │   └── advanced/
│   │       ├── __init__.py
│   │       ├── advanced_panel.py  # Advanced mode container
│   │       ├── wan_tab.py         # WAN configuration tab
│   │       ├── lora_tab.py        # LoRa configuration tab
│   │       ├── can_tab.py         # CAN configuration tab
│   │       ├── rs485_tab.py       # RS485 configuration tab

│   ├── serial/
│   │   ├── __init__.py
│   │   ├── manager.py        # Serial connection manager
│   │   └── reader.py         # Serial reader thread
│   └── utils/
│       ├── __init__.py
│       ├── logger.py         # Logging utilities
│       └── helpers.py        # Helper functions
└── tests/
    └── ...
```

### Class Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              GatewayConfigApp                               │
│─────────────────────────────────────────────────────────────────────────────│
│ - serial_manager: SerialManager                                             │
│ - config_manager: ConfigManager                                             │
│ - current_mode: str                                                         │
│─────────────────────────────────────────────────────────────────────────────│
│ + switch_mode(mode: str)                                                    │
│ + read_config()                                                             │
│ + write_config()                                                            │
│ + save_to_file()                                                            │
│ + load_from_file()                                                          │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ uses
                    ┌─────────────────┼─────────────────┐
                    ▼                 ▼                 ▼
          ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
          │  SerialManager  │ │  ConfigManager  │ │    UIManager    │
          │─────────────────│ │─────────────────│ │─────────────────│
          │ - port          │ │ - config_data   │ │ - main_window   │
          │ - baudrate      │ │ - original_data │ │ - panels        │
          │ - connection    │ │ - validators    │ │ - console       │
          │─────────────────│ │─────────────────│ │─────────────────│
          │ + connect()     │ │ + load()        │ │ + show_basic()  │
          │ + disconnect()  │ │ + save()        │ │ + show_advanced()│
          │ + send()        │ │ + validate()    │ │ + update_ui()   │
          │ + read()        │ │ + get_changes() │ │ + log()         │
          └─────────────────┘ └─────────────────┘ └─────────────────┘
```

---

## 🎨 Style Guide

### Color Palette

| Mục đích | Color | Hex Code |
|----------|-------|----------|
| Primary (Buttons, Headers) | Blue | `#2196F3` |
| Success | Green | `#4CAF50` |
| Warning | Orange | `#FF9800` |
| Error | Red | `#F44336` |
| Background | Light Gray | `#F5F5F5` |
| Card Background | White | `#FFFFFF` |
| Text Primary | Dark Gray | `#212121` |
| Text Secondary | Gray | `#757575` |
| Border | Light Gray | `#E0E0E0` |

### Typography

| Element | Font | Size | Weight |
|---------|------|------|--------|
| Window Title | Segoe UI | 16px | Bold |
| Section Header | Segoe UI | 12px | SemiBold |
| Label | Segoe UI | 10px | Regular |
| Input | Consolas | 10px | Regular |
| Console | Consolas | 9px | Regular |

### Icons (Unicode/Emoji)

| Action | Icon |
|--------|------|
| Connect | 🔌 |
| Disconnect | ⏏️ |
| Read | 📖 |
| Write | ✏️ |
| Save | 💾 |
| Load | 📂 |
| Success | ✅ |
| Error | ❌ |
| Warning | ⚠️ |
| Info | ℹ️ |
| WiFi | 📶 |
| Settings | ⚙️ |
| Clear | 🧹 |
| Firmware Update | 🔄 |
| Flash/Upload | ⬆️ |
| Progress | ⏳ |
| Cancel | ⏹️ |
| Browse File | 📂 |

---

## 📝 Implementation Phases

### Phase 1: Core Infrastructure (2-3 ngày)
- [ ] Setup project structure
- [ ] Implement SerialManager class
- [ ] Implement ConfigManager class
- [ ] Implement basic logging

### Phase 2: Main Window & Connection (2 ngày)
- [ ] Create main window layout với Split View (Configuration | Debug Log)
- [ ] Implement connection bar (COM chỉ hiện tên, nút Refresh text)
- [ ] Implement checkbox Advanced Mode (thay vì 2 nút to)
- [ ] Implement Debug Log panel (Application Log)

### Phase 3: Basic Mode (2-3 ngày)
- [ ] Create Basic mode tabbed interface (4 tabs)
- [ ] Implement WiFi tab với nút [Set]
- [ ] Implement LTE tab với nút [Set]
- [ ] Implement Server tab với nút [Set]
- [ ] Implement Interfaces tab với nút [Set]
- [ ] Add validation and feedback

### Phase 4: Advanced Mode (4-5 ngày)
- [ ] Create tabbed interface (6 tabs)
- [ ] Implement WAN tab với nút [Set] (LabelFrames không expand)
- [ ] Implement LoRa tab với nút [Set]
- [ ] Implement CAN tab với nút [Set] (không khoảng trống thừa)
- [ ] Implement RS485 tab với nút [Set] (không khoảng trống thừa)

- [ ] Implement Firmware Update tab

### Phase 5: Firmware Update Module (2-3 ngày)
- [ ] Implement FirmwareManager class
- [ ] Integrate esptool.py for flashing
- [ ] Create progress tracking UI
- [ ] Implement file validation (checksum, version)
- [ ] Add backup/restore functionality
- [ ] Test flash WAN MCU
- [ ] Test flash LAN MCU
- [ ] Test flash both MCUs sequentially

### Phase 6: Integration & Testing (2-3 ngày)
- [ ] Connect UI to backend
- [ ] Test serial communication
- [ ] Test config read/write
- [ ] Test firmware update end-to-end
- [ ] Fix bugs and polish UI

### Phase 7: Documentation & Release (1-2 ngày)
- [ ] Write user guide
- [ ] Create installer/executable
- [ ] Bundle esptool and flash scripts
- [ ] Final testing

---

## 🔧 Dependencies

```txt
# requirements.txt
tkinter          # GUI (built-in)
pyserial>=3.5    # Serial communication
ttkthemes>=3.2   # Modern themes for tkinter
pillow>=9.0      # Image handling for icons
esptool>=4.7     # ESP32 firmware flashing tool
```

---

## � COMMAND REFERENCE (Từ Firmware)

### Nhóm 1: WAN MCU Commands (gửi trực tiếp qua UART)

| Command | Format | Mô tả | Default Value |
|---------|--------|-------|---------------|
| `CFIN` | `CFIN:TYPE` | Set internet type | `WIFI` |
| | Options: `WIFI`, `LTE` | | |
| `CFWF` | `CFWF:SSID:PASSWORD:AUTH_MODE` | WiFi config (gộp) | ssid="", password="" |
| | AUTH_MODE: `PERSONAL`, `ENTERPRISE` | Nếu ENTERPRISE thêm username | |
| `CFLT` | `CFLT:COMM:APN:USER:PASS:RECONNECT:TIMEOUT:RETRY` | LTE config (gộp) | apn="internet" |
| | COMM: `UART`, `USB` | | |
| `CFMQ` | `CFMQ:BROKER\|TOKEN\|SUB\|PUB\|ATTR` | MQTT config (gộp) | broker="mqtt://localhost:1883" |
| | Dùng `\|` để ngăn cách (pipe) | | |
| `CFSV` | `CFSV:TYPE` | Server type | `MQTT` |
| | Options: `MQTT`, `HTTP`, `COAP` | | |

### Nhóm 2: LAN MCU Commands (gửi trực tiếp, không prefix)

| Command | Format | Mô tả | Default Value |
|---------|--------|-------|---------------|
| `CFCB` | `CFCB:baudrate` | CAN baud rate | `500000` |
| | Valid: `125000`, `250000`, `500000`, `800000`, `1000000` | | |
| `CFCM` | `CFCM:mode` | CAN operating mode | `NORMAL` |
| | Valid: `NORMAL`, `LOOPBACK`, `NO_ACK` | | |
| `CFCW:ADD` | `CFCW:ADD:0xXXX` | Add CAN ID to whitelist | - |
| `CFCW:REM` | `CFCW:REM:0xXXX` | Remove CAN ID | - |
| `CFCW:CLR` | `CFCW:CLR` | Clear whitelist | - |
| `CFCW:SET` | `CFCW:SET:0xXXX,0xYYY` | Set whitelist (replace) | - |
| `CFST` | `CFST:ST_1:TYPE` | Stack 1 type | `NONE` |
| | `CFST:ST_2:TYPE` | Stack 2 type | `NONE` |
| | Valid: `NONE`, `LORA`, `RS485`, `ZIGBEE`, `CAN` | | |
| `CFRS:BR` | `CFRS:BR:baudrate` | RS485 baud | `115200` |
| | Valid: `9600`, `19200`, `38400`, `57600`, `115200` | | |

### Nhóm 3: LoRa Commands (Binary, prefix `CFLR:`)

| Command | Format | Mô tả |
|---------|--------|-------|
| `CFLR:MODEM` | `CFLR:MODEM:<6 bytes>` | LoRa E32 modem params |
| | Bytes: head, addh, addl, sped, chan, option | |
| `CFLR:HDLCF` | `CFLR:HDLCF:<11 bytes>` | LoRa TDMA config |
| | role(1), node_id(2), gw_id(2), slots(1), my_slot(1), slot_ms(4) | |
| `CFLR:CRYPT` | `CFLR:CRYPT:<len><key>` | LoRa encryption key |
| | len: 1-32 bytes | |

### Nhóm 4: FOTA Commands

| Command | Format | Mô tả |
|---------|--------|-------|
| `CFFW` | `CFFW` hoặc `CFFW:URL:FORCE` | LAN MCU firmware update |

---

## �📌 Notes cho Developer

### Validation Rules

1. **WiFi SSID**: Max 32 characters, không rỗng
2. **WiFi Password**: Min 8 characters (WPA2)
3. **MQTT Broker**: Must match URL pattern `mqtt://` or `mqtts://`
4. **CAN ID**: Range 0x000-0x7FF (standard) hoặc 0x00000000-0x1FFFFFFF (extended)
5. **Hex Input**: Must start with `0x` hoặc auto-prepend

### Error Handling

- Hiển thị lỗi inline dưới input field
- Không cho phép submit nếu có validation errors
- Log tất cả errors vào console

### Security

- Mask password fields
- Không log password values
- Warn khi save config với password to file

### CFSC Protocol Details

#### Response Format
Khi PC gửi lệnh `CFSC`, Gateway sẽ trả về config theo format:

```
CFSC_RESP:START

[GATEWAY_INFO]
key1=value1
key2=value2

[WAN_CONFIG]
key1=value1
key2=value2

[LAN_CONFIG]
key1=value1
key2=value2

CFSC_RESP:END
```

#### Line Ending
- `\r\n` (Windows style)

#### Special Values
- Passwords: `***HIDDEN***` (never show plain text)
- Unavailable data: `UNAVAILABLE`
- Read-only fields: Có marker `(Read-only)` trong UI

#### Timeout
- UART read timeout: 5 seconds
- LAN config request timeout: 3 seconds

#### Error Handling
- Nếu không nhận được `CFSC_RESP:END` → Show error
- Nếu LAN config timeout → Show `lan_status=UNAVAILABLE`
- Validate response format trước khi parse