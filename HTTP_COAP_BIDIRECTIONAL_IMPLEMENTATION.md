# HTTP & CoAP Bidirectional Implementation

## Overview

This implementation adds RPC command reception and response routing capabilities to HTTP and CoAP handlers, making them work like MQTT with full bidirectional communication.

**Completed Changes:**
1. ✅ Added CMD_SOURCE_COAP and CMD_SOURCE_HTTP_RPC to command source enum
2. ✅ Implemented HTTP RPC polling mechanism
3. ✅ Implemented CoAP RPC polling mechanism  
4. ✅ Added RPC ID tracking for both handlers
5. ✅ Added command routing to config_handler_queue
6. ✅ Added response routing back to appropriate endpoints
7. ✅ Updated MCU LAN handler to recognize new source types

---

## Architecture

### Command Flow Diagram

```
ThingsBoard Server
    ├── MQTT: Publishes RPC to v1/devices/me/rpc/request/{id}
    ├── HTTP: Stores pending RPC (GET /api/v1/{token}/rpc)
    └── CoAP: Sends RPC notifications (OBSERVE or GET /rpc)
           ↓
    WAN Gateway (ESP32-S3)
        ├── MQTT Handler
        │   ├── Receives RPC via MQTT subscribe
        │   ├── Stores g_last_rpc_id
        │   └── Routes to config_handler via mqtt_receive_enqueue()
        │
        ├── HTTP Handler (NEW)
        │   ├── Polling Task (every 5s)
        │   │   ├── GET /api/v1/{token}/rpc
        │   │   ├── Parse RPC ID from response
        │   │   ├── Extract command hex string: CF...
        │   │   └── Enqueue to config_handler_queue (CMD_SOURCE_HTTP_RPC)
        │   └── Publish Task
        │       ├── Converts binary → hex → JSON
        │       ├── Checks s_last_rpc_id
        │       └── Routes to /v1/devices/me/rpc/response/{id} if RPC response
        │
        └── CoAP Handler (NEW)
            ├── Polling Task (every 5s)
            │   ├── GET coap://host/resource/rpc
            │   ├── Parse RPC ID from response
            │   ├── Extract command hex string: CP...
            │   └── Enqueue to config_handler_queue (CMD_SOURCE_COAP)
            └── Publish Task
                ├── Converts binary → hex
                ├── Sends via CoAP POST (confirmable)
                └── Responses routed back via server handler

    Config Handler Task
        ├── Receives command_t from queue
        ├── Routes by type (CONFIG_TYPE_COAP, CONFIG_TYPE_HTTP, etc.)
        ├── Updates respective handler config
        └── Config handlers apply new settings

    MCU LAN Handler (Uplink)
        ├── Processes LAN MCU responses
        ├── Routes based on cmd->source:
        │   ├── UART/USB → Send back to interface
        │   ├── HTTP → Log locally
        │   ├── HTTP_RPC → Forward to server handler
        │   └── COAP → Forward to server handler
        └── MQTT/HTTP_RPC/COAP → server_handler_enqueue_uplink()
                                   ↓
                            MQTT Publish: telemetry or RPC response
```

---

## Implementation Details

### 1. Command Source Types (mcu_lan_handler.h)

```c
typedef enum {
  CMD_SOURCE_MQTT     = 0,    // From MQTT RPC
  CMD_SOURCE_UART     = 1,    // From UART PC App
  CMD_SOURCE_USB      = 2,    // From USB Serial JTAG
  CMD_SOURCE_HTTP     = 3,    // From Web Config Portal
  CMD_SOURCE_COAP     = 4,    // From CoAP OBSERVE/polling
  CMD_SOURCE_HTTP_RPC = 5,    // From HTTP RPC polling
  CMD_SOURCE_UNKNOWN  = 0xFF
} command_source_t;
```

### 2. HTTP Handler Polling (http_handler.c)

#### Features:
- **Polling Interval**: 5 seconds (configurable via HTTP_RPC_POLL_INTERVAL_MS)
- **Endpoint**: GET `/api/v1/{token}/rpc`
- **Response Parsing**: Extracts JSON `params` field containing hex-encoded command
- **Command Formatting**: Converts hex string to binary, validates `CF` prefix
- **Queue Integration**: Enqueues to `g_config_handler_queue` with `CMD_SOURCE_HTTP_RPC`
- **Response Routing**: Publishes responses to `/v1/devices/me/rpc/response/{rpc_id}`

#### New Functions:
```c
static esp_err_t http_poll_rpc(void)           // Single poll operation
static void http_polling_task(void *arg)       // Periodic polling task
static esp_err_t http_extract_rpc_params(...)  // Parse JSON params
```

#### New Variables:
```c
static TaskHandle_t  s_polling_task_handle;    // Polling task handle
static bool          s_polling_running;        // Polling task state
static int           s_last_rpc_id;            // Track RPC response ID
#define HTTP_RPC_POLL_INTERVAL_MS 5000         // Poll interval
```

### 3. CoAP Handler Polling (coap_handler.c)

#### Features:
- **Polling Interval**: 5 seconds (configurable via COAP_RPC_POLL_INTERVAL_MS)
- **Endpoint**: CoAP GET `/resource/{token}/rpc`
- **Session Type**: Can use DTLS (CoAPS) or plain UDP
- **Command Parsing**: Similar hex extraction as HTTP
- **Queue Integration**: Enqueues to `g_config_handler_queue` with `CMD_SOURCE_COAP`
- **Response Routing**: Responses sent via server handler (MQTT or HTTP)

#### New Functions:
```c
static esp_err_t coap_poll_rpc(void)           // Single poll operation
static void coap_polling_task(void *arg)       // Periodic polling task
```

#### New Variables:
```c
static TaskHandle_t  s_polling_task_handle;    // Polling task handle
static bool          s_polling_running;        // Polling task state
static int           s_last_rpc_id;            // Track RPC response ID
#define COAP_RPC_POLL_INTERVAL_MS 5000         // Poll interval
```

### 4. Response Routing Updates

#### HTTP Response Path:
```
config_handler processes command
  ↓
command->source == CMD_SOURCE_HTTP_RPC
  ↓
Telemetry published from MCU LAN
  ↓
http_publish_task receives data
  ↓
s_last_rpc_id >= 0?
  ├─ YES: POST to /v1/devices/me/rpc/response/{rpc_id}
  └─ NO: POST to telemetry endpoint
```

#### CoAP Response Path:
```
config_handler processes command
  ↓
command->source == CMD_SOURCE_COAP
  ↓
Telemetry published from MCU LAN
  ↓
coap_publish_task receives data
  ↓
server_handler_enqueue_uplink() → MQTT or HTTP forward
```

---

## Testing & Verification

### Test Case 1: HTTP RPC Command Reception
1. Configure gateway with HTTP server type: `CFSV:1`
2. From ThingsBoard, send RPC method with params
3. Expect: HTTP polling task detects pending RPC within 5s
4. Verify logs: `"Extracted RPC params:"` and `"HTTP RPC command enqueued"`
5. Expect: config_handler processes command with `CMD_SOURCE_HTTP_RPC`

### Test Case 2: CoAP RPC Command Reception
1. Configure gateway with CoAP server type: `CFSV:2`
2. Send CoAP RPC command to `/resource/{token}/rpc` endpoint
3. Expect: CoAP polling task detects pending command within 5s
4. Verify logs: `"Extracted RPC params:"` and `"CoAP RPC command enqueued"`
5. Expect: config_handler processes command with `CMD_SOURCE_COAP`

### Test Case 3: HTTP RPC Response Routing
1. Send WiFi config via HTTP RPC
2. LAN MCU processes and responds
3. Expect: Response routed to `/v1/devices/me/rpc/response/{id}` not telemetry
4. Verify: HTTP status 200 OK for RPC response

### Test Case 4: Command Types Supported
All configuration types continue to work:
- `CFWF:` - WiFi config
- `CFMQ:` - MQTT config
- `CFCP:` - CoAP config
- `CFHP:` - HTTP config
- `CFLT:` - LTE config
- etc.

---

## Debugging

### Enable Debug Logging:
```c
// In http_handler.c or coap_handler.c
esp_log_level_set(TAG, ESP_LOG_DEBUG);
esp_log_level_set("MCU_LAN_UL", ESP_LOG_DEBUG);
```

### Key Log Messages:

#### HTTP Polling:
```
I (XXX) HTTP_HANDLER: Polling RPC from: http://...
I (XXX) HTTP_HANDLER: HTTP GET RPC response: status=200, len=XX
I (XXX) HTTP_HANDLER: Extracted RPC params: CF...
I (XXX) HTTP_HANDLER: HTTP RPC command enqueued to config handler
```

#### CoAP Polling:
```
I (XXX) COAP_HANDLER: Polling RPC from: coap://host:5683/resource/rpc
I (XXX) COAP_HANDLER: CoAP RPC polling completed
```

#### Config Handler Processing:
```
I (XXX) config_handler: Received config command, type: X, len: YYY
I (XXX) config_handler: Server type config command received
I (XXX) config_handler: Server type updated to: 1
```

---

## Known Limitations & Future Improvements

### Current Limitations:
1. **HTTP Polling**: Uses simple GET polling (5s interval), not long-polling
2. **CoAP Polling**: Uses GET polling with timeout, not OBSERVE option
3. **Command Parsing**: Assumes simple hex string in params, doesn't handle complex nested JSON
4. **Resource Paths**: Hardcoded `/rpc` suffix, assumes ThingsBoard API structure
5. **No Subscription**: Polling is active, not reactive (consumes more bandwidth)

### Recommended Improvements:
1. **HTTP Long-Polling**: Implement HTTP long-polling with configurable timeout
2. **CoAP OBSERVE**: Add COAP_OPTION_OBSERVE for push notifications
3. **Configurable Polling Interval**: Move poll intervals to config structure
4. **Better JSON Parsing**: Use cJSON library for complex RPC payloads
5. **Offline Queue**: Cache commands when offline, process on reconnection

---

## Configuration Notes

### ThingsBoard Setup:
1. **HTTP Endpoint**: Configure telemetry URL like:
   ```
   http://gateway-ip:port/api/v1/{token}/telemetry
   ```
   RPC polling will use: `http://gateway-ip:port/api/v1/{token}/rpc`

2. **CoAP Endpoint**: Configure device parameters:
   ```
   Host: coap.thingsboard.io
   Port: 5783 (DTLS) or 5683 (plain)
   Resource: /api/v1/{token}
   ```
   RPC polling will use: `coap://device.example.com/api/v1/{token}/rpc`

3. **RPC Method Names**: No special names required, any RPC can be sent

---

## Files Modified

| File | Changes |
|------|---------|
| `MCU_LAN_Handler/include/mcu_lan_handler.h` | Added CMD_SOURCE_COAP, CMD_SOURCE_HTTP_RPC enums |
| `MCU_LAN_Handler/src/mcu_lan_handler_uplink.c` | Updated source routing logs |
| `Server_Communication_Handler/http_handler/src/http_handler.c` | Added polling task & RPC management |
| `Server_Communication_Handler/http_handler/include/http_handler.h` | (No changes, internal only) |
| `Server_Communication_Handler/coap_handler/src/coap_handler.c` | Added polling task & RPC management |
| `Server_Communication_Handler/coap_handler/include/coap_handler.h` | (No changes, internal only) |

---

## Verification Checklist

- [ ] Code compiles without errors
- [ ] HTTP polling task starts when HTTP handler initialized
- [ ] CoAP polling task starts when CoAP handler initialized
- [ ] RPC commands from ThingsBoard detected within polling interval
- [ ] Commands routed to config_handler with correct source type
- [ ] Responses routed back to appropriate endpoint
- [ ] Multiple commands in queue processed correctly
- [ ] Graceful handling of offline state
- [ ] Memory allocations cleaned up properly
- [ ] No stack overflow in polling tasks (6KB/4KB stack)
