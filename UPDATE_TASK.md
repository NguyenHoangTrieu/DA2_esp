# MODULE BASE SETTING - UPDATE TASK LIST

## OVERVIEW - SYSTEM ARCHITECTURE

### Core Concept
The baseboard gateway is a TRANSPARENT BRIDGE between PC App/Server and wireless modules (BLE, Zigbee, LoRa).
It receives commands, translates them using JSON config, forwards to module, captures response, and sends back.

### Data Flow Architecture (7 Flows)

#### Flow 1 - Module Configuration (JSON Config)
```
PC App -> UART -> WAN MCU -> SPI -> LAN MCU -> JSON Parser -> Apply Config
```
- WAN MCU receives JSON string via UART
- Forwards entire JSON via SPI to LAN MCU
- LAN MCU parses JSON and stores function mappings
- No response needed, just ACK

#### Flow 2 - Sensor Data Uplink
```
Module -> LAN MCU -> SPI -> WAN MCU -> UART/MQTT/HTTP -> Server
```
- Sensor data flows continuously
- LAN MCU frames data with handler ID
- WAN MCU routes to appropriate protocol (UART for app, MQTT/HTTP for server)

#### Flow 3 - Module Control from Server
```
Server -> MQTT/HTTP -> WAN MCU -> SPI -> LAN MCU -> Execute Function -> Response -> Server
```
- Server sends control command via MQTT/HTTP
- WAN MCU frames as CONFIG command and sends via SPI
- LAN MCU executes function based on stored JSON config
- Response sent back through same path

#### Flow 4 - Scan/Discovery from App
```
App -> UART -> WAN MCU -> SPI -> LAN MCU -> Execute SCAN -> Stream Results -> UART -> App
```
- App sends scan command (e.g. AT+SCAN=5000)
- LAN MCU starts scan based on JSON config
- ALL responses from module streamed back to app in real-time
- Timeout based on scan duration

#### Flow 5 - Setup Commands from App
```
App -> UART -> WAN MCU -> SPI -> LAN MCU -> Execute Setup -> Response -> UART -> App
```
- App sends setup command (e.g. AT+NAME=Device1)
- LAN MCU executes using JSON config mapping
- Single response validation and sent back

#### Flow 6 - Scan/Discovery from Server
```
Server -> MQTT/HTTP -> WAN MCU -> SPI -> LAN MCU -> Execute SCAN -> Collect Results -> Server
```
- Same as Flow 4 but from server
- Results batched and sent back via MQTT/HTTP/CoAP

#### Flow 7 - Setup Commands from Server
```
Server -> MQTT/HTTP -> WAN MCU -> SPI -> LAN MCU -> Execute Setup -> Response -> Server
```
- Same as Flow 5 but from server
- Response sent back via MQTT/HTTP/CoAP

---

## CURRENT ISSUES IDENTIFIED

### Issue 1 - GPIO-Only Functions Rejected
**Location:** `DA2_esp_LAN/Middleware/BLE_Handler/src/ble_handler.c`
**Problem:** Functions with empty command string fail validation
**Affected Functions:** MODULE_HW_RESET, MODULE_ENTER_CMD_MODE, MODULE_ENTER_DATA_MODE, MODULE_WAKEUP
**Impact:** These functions cannot execute, system cannot reset module or change modes

### Issue 2 - Function Name Mismatch
**Location:** `DA2_esp_LAN/Middleware/JSON_Config_Parser/src/json_ble_config_parser.c`
**Problem:** Parser expects MODULE_SET_SECURITY and MODULE_MANAGE_WHITELIST
**TODO Specifies:** MODULE_SET_SECURITY_CONFIG and MODULE_ENTER_BOOTLOADER
**Impact:** JSON files using TODO names will be rejected

### Issue 3 - No Response Functions Still Block
**Location:** `DA2_esp_LAN/Middleware/BLE_Handler/src/ble_handler.c`
**Problem:** module_bus_read always waits for timeout_ms even if expect_response is empty
**Impact:** GPIO-only functions that should return immediately will wait unnecessarily

### Issue 4 - No Streaming Response Mechanism
**Location:** All BLE handler layers
**Problem:** Single response capture only, no continuous streaming for SCAN results
**Impact:** Flow 4 and Flow 6 (scan/discovery) cannot work correctly

### Issue 5 - BLE Command Path Disabled
**Location:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**Problem:** CONFIG_UPDATE_BLE_JSON, CONFIG_UPDATE_BLE_DISC, CONFIG_UPDATE_BLE_SETUP are commented out
**Impact:** Flows 1, 4, 5, 6, 7 do not work for BLE modules

### Issue 6 - No Response Forwarding to App
**Location:** MCU WAN Handler downlink path
**Problem:** Module responses not automatically forwarded back to WAN MCU for app delivery
**Impact:** App cannot receive scan results or command responses

---

## IMPLEMENTATION TASKS

### Task Group 1 - Fix Core BLE Handler Issues (HIGH PRIORITY)

#### Task 1.1 - Allow GPIO-Only Functions
**File:** `DA2_esp_LAN/Middleware/BLE_Handler/src/ble_handler.c`
**Function:** `ble_execute_function_internal()`
**Changes:**
1. Check if command string is empty BEFORE validation
2. If empty AND expect_response is empty:
   - Execute GPIO start sequences
   - Wait delay_start_ms
   - Execute GPIO end sequences
   - Wait delay_end_ms
   - Return ESP_OK immediately (no bus write/read)
3. If command is NOT empty:
   - Continue with existing validation and execution

**Logic:**
```
if strlen(final_command) == 0:
    if strlen(expect_response) == 0:
        // GPIO-only function
        execute_gpio_start()
        delay_start()
        execute_gpio_end()
        delay_end()
        return ESP_OK
    else:
        // Error: no command but expects response
        return ESP_ERR_INVALID_ARG
else:
    // Normal command execution
    validate_command()
    execute_gpio_start()
    send_command()
    wait_response()
    validate_response()
    execute_gpio_end()
```

#### Task 1.2 - Fix Function Name Alignment
**File:** `DA2_esp_LAN/Middleware/JSON_Config_Parser/src/json_ble_config_parser.c`
**Array:** `BLE_FUNCTION_NAMES[JSON_BLE_FUNC_MAX]`
**Changes:**
Replace index 18 and 19:
- OLD: MODULE_SET_SECURITY, MODULE_MANAGE_WHITELIST
- NEW: MODULE_SET_SECURITY_CONFIG, MODULE_ENTER_BOOTLOADER

**File:** `DA2_esp_LAN/Middleware/BLE_Handler/include/ble_handler.h`
**Enum:** `ble_function_id_t`
**Changes:**
Update enum names to match:
```
BLE_FUNC_SET_SECURITY_CONFIG = 18,
BLE_FUNC_ENTER_BOOTLOADER = 19,
```

#### Task 1.3 - Optimize No-Response Timeout
**File:** `DA2_esp_LAN/Middleware/BLE_Handler/src/ble_handler.c`
**Function:** `ble_execute_function_internal()`
**Changes:**
1. Before calling module_bus_read(), check:
   - If expect_response is empty AND timeout_ms == 0:
     - Skip module_bus_read() entirely
     - Move to GPIO end sequences immediately
2. This allows JSON to specify timeout=0 for no-wait functions

**Logic:**
```
if strlen(expect_response) == 0 AND timeout_ms == 0:
    // No response expected, skip read
    goto gpio_end_sequence
else:
    // Normal response handling
    module_bus_read(timeout_ms)
    validate_response()
```

### Task Group 2 - Add Response Streaming Support (HIGH PRIORITY)

#### Task 2.1 - Add Streaming Mode to BLE Handler
**File:** `DA2_esp_LAN/Middleware/BLE_Handler/src/ble_handler.c`
**New Function:** `ble_execute_function_streaming()`
**Purpose:** Execute command and stream ALL responses back to caller

**Parameters:**
```
esp_err_t ble_execute_function_streaming(
    uint8_t stack_id,
    ble_function_id_t func_id,
    const char *param,
    uint32_t stream_duration_ms,
    ble_stream_callback_t callback,
    void *user_data
);

typedef void (*ble_stream_callback_t)(
    const uint8_t *data,
    uint16_t len,
    void *user_data
);
```

**Logic:**
1. Execute GPIO start and send command (same as normal)
2. Instead of single module_bus_read():
   - Loop for stream_duration_ms:
     - Call module_bus_read() with short timeout (50ms)
     - If data received, call callback immediately
     - Callback can forward data to WAN MCU
     - Continue until stream_duration_ms expires
3. Execute GPIO end sequences

#### Task 2.2 - Add Response Forwarding in Config Handler
**File:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**New Functions:**
```
config_parse_ble_scan()    // For SCAN commands
config_parse_ble_command() // For generic AT commands with streaming
```

**Streaming Callback:**
```
static void stream_response_to_wan(const uint8_t *data, uint16_t len, void *user_data) {
    // Frame response packet for WAN MCU
    uint8_t packet[256];
    packet[0] = 'B';  // BLE response marker
    packet[1] = 'R';  // Response marker
    memcpy(&packet[2], data, len);
    
    // Send to WAN via uplink queue
    mcu_wan_enqueue_uplink(HANDLER_BLE, packet, len + 2);
}
```

### Task Group 3 - Enable BLE Command Path (MEDIUM PRIORITY)

#### Task 3.1 - Uncomment BLE Config Handlers
**File:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**Changes:**
1. Uncomment config_parse_ble_json() function
2. Uncomment config_parse_ble_discovery() function
3. Uncomment config_parse_ble_setup() function
4. Uncomment case blocks in config_handler_task():
   - CONFIG_UPDATE_BLE_JSON
   - CONFIG_UPDATE_BLE_DISC
   - CONFIG_UPDATE_BLE_SETUP

**WARNING:** These changes should be kept in COMMENTS until testing is complete.
Add comment block: /* PENDING TEST - DO NOT ACTIVATE YET */

#### Task 3.2 - Enable BLE Downlink Dispatch
**File:** `DA2_esp_LAN/Application/MCU_WAN_Handler/src/mcu_wan_handler_downlink.c`
**Function:** `dispatch_downlink_to_handler()`
**Changes:**
Uncomment:
```
case HANDLER_BLE:
    success = ble_handler_task_enqueue_downlink(data, length);
    break;
```

**WARNING:** Keep in COMMENTS until Task 3.1 is tested.

### Task Group 4 - Add Command Parsing for All Flows (HIGH PRIORITY)

#### Task 4.1 - Parse Prefix for Streaming Commands
**File:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**New Function:** `config_parse_ble_scan()`

**Format:** `CFBL:SCAN:<prefix>:<timeout>:<stack_id>`
**Example:** `CFBL:SCAN:AT+SCAN=:5000:0`

**Logic:**
1. Extract prefix (e.g. AT+SCAN=)
2. Extract timeout (5000ms)
3. Extract stack_id (0 or 1)
4. Build full command: prefix + param (if any)
5. Call ble_execute_function_streaming() with callback
6. Callback forwards all +SCAN: responses to WAN MCU

#### Task 4.2 - Parse Setup Commands
**File:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**Update Function:** `config_parse_ble_setup()`

**Current Format:** `CFBL:SETUP:<function_id>:<stack_id>:<params>`
**Keep format, but add response forwarding:**

**Logic:**
1. Parse function_id (maps to JSON function)
2. Parse stack_id
3. Parse params
4. Call ble_handler_execute_function()
5. Forward result.response to WAN MCU immediately
6. Format: `BR:SETUP:<func_id>:<status>:<response>`

#### Task 4.3 - Add Generic Command Handler
**File:** `DA2_esp_LAN/Application/Config_Handler/src/config_handler.c`
**New Function:** `config_parse_ble_command()`

**Format:** `CFBL:CMD:<command>:<expect>:<timeout>:<stack_id>`
**Example:** `CFBL:CMD:AT+VER:JDY-23:500:0`

**Purpose:** Allow app to send arbitrary AT commands with expected response

---

## IMPLEMENTATION SEQUENCE

### Phase 1 - Core Fixes (Week 1)
1. Task 1.1 - Allow GPIO-only functions
2. Task 1.2 - Fix function name alignment
3. Task 1.3 - Optimize no-response timeout
4. Test with GPIO-only functions (HW_RESET, WAKEUP)

### Phase 2 - Streaming Support (Week 2)
1. Task 2.1 - Add streaming mode to BLE handler
2. Task 2.2 - Add response forwarding in config handler
3. Task 4.1 - Parse prefix for streaming commands
4. Test Flow 4 (scan from app)

### Phase 3 - Enable Command Path (Week 3)
1. Task 4.2 - Parse setup commands
2. Task 4.3 - Add generic command handler
3. Test Flow 5 (setup from app)
4. KEEP Task 3.1 and 3.2 COMMENTED until Phase 4

### Phase 4 - Integration Testing (Week 4)
1. Uncomment Task 3.1 (BLE config handlers)
2. Uncomment Task 3.2 (BLE downlink dispatch)
3. Test all 7 flows end-to-end
4. Performance testing

---

## TESTING CHECKLIST

### Flow 1 - JSON Config
- [ ] Send BLE JSON via app UART
- [ ] WAN forwards to LAN via SPI
- [ ] LAN parses JSON successfully
- [ ] Functions available in ble_handler
- [ ] ACK returned to app

### Flow 2 - Sensor Data
- [ ] BLE device sends data
- [ ] LAN frames with HANDLER_BLE
- [ ] WAN receives via SPI
- [ ] Data forwarded to UART (app) or MQTT (server)

### Flow 3 - Control from Server
- [ ] Server sends control via MQTT
- [ ] WAN frames and sends via SPI
- [ ] LAN executes function
- [ ] Response sent back via MQTT

### Flow 4 - Scan from App
- [ ] App sends CFBL:SCAN:AT+SCAN=:5000:0
- [ ] LAN executes streaming scan
- [ ] ALL +SCAN: responses forwarded to app
- [ ] Timeout after 5000ms
- [ ] Final result summary sent

### Flow 5 - Setup from App
- [ ] App sends CFBL:SETUP:4:0:TestDevice (SET_NAME)
- [ ] LAN executes function
- [ ] Response forwarded to app
- [ ] App displays OK or FAIL

### Flow 6 - Scan from Server
- [ ] Server sends scan command via MQTT
- [ ] Same as Flow 4 but via MQTT
- [ ] Results batched and sent back via MQTT

### Flow 7 - Setup from Server
- [ ] Server sends setup via MQTT
- [ ] Same as Flow 5 but via MQTT
- [ ] Response sent back via MQTT

### GPIO-Only Functions Test
- [ ] MODULE_HW_RESET (GPIO toggle only)
- [ ] MODULE_WAKEUP (GPIO high with delay)
- [ ] MODULE_ENTER_CMD_MODE (no command)
- [ ] MODULE_ENTER_DATA_MODE (no command)
- [ ] All execute without blocking or errors

### Response Classification Test
- [ ] Functions with expect_response="" and timeout=0 return immediately
- [ ] Functions with expect_response="OK" wait for response
- [ ] Functions with expect_response="" and timeout>0 wait for timeout
- [ ] Streaming functions collect all responses

---

## IMPORTANT NOTES

### Code Safety Rules
1. DO NOT activate config_handler BLE functions until Phase 4
2. Keep /* PENDING TEST - DO NOT ACTIVATE YET */ comments
3. Test each phase independently before proceeding
4. Document any deviations from this task list

### File Modification Warnings
**Files that need special care:**
- `config_handler.c` - Keep BLE handlers commented until Phase 4
- `mcu_wan_handler_downlink.c` - Keep BLE dispatch commented until Phase 4
- `server_communication_handler.c` - Not modified in this task (future work)

### JSON Config Examples
After implementation, gateway should work with these JSON configs:

**GPIO-only function:**
```json
{
  "function_name": "MODULE_HW_RESET",
  "command": "",
  "gpio_start_control": [{"pin": "01", "state": "LOW"}],
  "delay_start": 100,
  "expect_response": "",
  "timeout": 0,
  "gpio_end_control": [{"pin": "01", "state": "HIGH"}],
  "delay_end": 500
}
```

**Streaming SCAN function:**
```json
{
  "function_name": "MODULE_START_DISCOVERY",
  "command": "AT+SCAN={PARAM}",
  "gpio_start_control": [],
  "delay_start": 0,
  "expect_response": "+SCAN:",
  "timeout": 5000,
  "gpio_end_control": [],
  "delay_end": 0
}
```

**Setup function with response:**
```json
{
  "function_name": "MODULE_SET_NAME",
  "command": "AT+NAME={PARAM}",
  "gpio_start_control": [],
  "delay_start": 0,
  "expect_response": "OK",
  "timeout": 500,
  "gpio_end_control": [],
  "delay_end": 0
}
```

---

## SUCCESS CRITERIA

System is considered complete when:
1. All 7 data flows work end-to-end
2. GPIO-only functions execute correctly
3. Streaming responses delivered to app in real-time
4. Setup commands return responses to app
5. JSON config files control all module behavior
6. No hardcoded AT commands in firmware
7. Gateway transparent to app and server

---

## ARCHITECTURE SUMMARY

```
+--------+                    +----------+                    +----------+
|        | <---- UART ----->  |          | <---- SPI ----->  |          |
| PC APP/|
| Server |                    | WAN MCU  |                    | LAN MCU  |
|        | <---- MQTT ----->  |(Gateway) |                    |(Baseboard)|
+--------+                    +----------+                    +----------+
                                                                    |
                                                              JSON Config
                                                              Mapping Engine
                                                                    |
                                                    +---------------+---------------+
                                                    |               |               |
                                                  BLE           Zigbee          LoRa
                                                 Module         Module         Module
```

**Key Principle:** Gateway does NOT understand module commands.
It only knows:
1. How to read JSON config
2. How to toggle GPIO pins
3. How to send bytes over UART/SPI/I2C/USB
4. How to wait for response substring
5. How to forward responses back to app/server

Module-specific knowledge (AT commands, timing, protocols) lives in JSON config file, NOT in firmware.

---

END OF UPDATE TASK LIST
