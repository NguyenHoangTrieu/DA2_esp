# QSPI + SD Card Communication Design Document
## Modular Architecture with Mandatory QSPI Interface

---

## 1. SYSTEM OVERVIEW

Dual ESP32 architecture with **mandatory QSPI (Quad SPI)** for high-speed bidirectional communication and modular task separation.

**Key Features**:
- QSPI: 40 MHz, 4-bit parallel (20 MB/s theoretical, 8-12 MB/s practical)
- LAN MCU (Master): Protocol handlers (CAN/LoRa/ZigBee/RS485) + SD card storage
- WAN MCU (Slave): WiFi/MQTT + RTC (PCF8563)
- Uplink throughput: 800 KB/s sustained, 2 MB/s burst
- Downlink latency: <5 ms (ISR to data received)

---

## 2. HARDWARE ARCHITECTURE

### 2.1. QSPI Bus (Mandatory - 40 MHz)

| Signal | LAN MCU (Master) | WAN MCU (Slave) | GPIO | Purpose |
|--------|------------------|-----------------|------|---------|
| CLK | Output | Input | GPIO12 | 40 MHz clock |
| CS | Output | Input | GPIO10 | Chip select |
| IO0 (D0) | Bidirectional | Bidirectional | GPIO11 | Data line 0 |
| IO1 (D1) | Bidirectional | Bidirectional | GPIO13 | Data line 1 |
| IO2 (D2) | Bidirectional | Bidirectional | GPIO14 | Data line 2 |
| IO3 (D3) | Bidirectional | Bidirectional | GPIO15 | Data line 3 |
| DR_LAN | Input (ISR) | - | GPIO46 | Data-ready from WAN |
| DR_WAN | - | Output | GPIO8 | Data-ready to LAN |

### 2.2. SD Card Interface (LAN MCU Only)

| Pin | Function | GPIO | Notes |
|-----|----------|------|-------|
| CLK | SDMMC Clock | GPIO7 | Separate from QSPI |
| CMD | Command | GPIO6 | |
| D0-D3 | Data Lines | GPIO8,3,4,5 | 4-bit mode |

**No Pin Conflicts**: QSPI (GPIO10-15) vs SD (GPIO3-8)

---

## 3. MODULAR FILE STRUCTURE

### 3.1. LAN MCU (Master) - 8 Files

mcu_wan_handler.h # Public API
mcu_wan_handler_downlink.c # Downlink task (Priority 7)
mcu_wan_handler_uplink.c # Uplink task (Priority 5)
storage_handler.h/c # SD card abstraction
wan_comm.h/c # QSPI master driver
frame_types.h # Protocol definitions

text

### 3.2. WAN MCU (Slave) - 6 Files

mcu_lan_handler.h # Public API
mcu_lan_handler_uplink.c # Uplink processor (Priority 6)
mcu_lan_handler_downlink.c # Downlink manager (no task)
lan_comm.h/c # QSPI slave driver
frame_types.h # Protocol definitions

text

---

## 4. MODULE RESPONSIBILITIES

### 4.1. LAN MCU

**mcu_wan_handler_downlink.c** (Priority 7):
- GPIO46 ISR response (<5ms latency)
- DQ retry (10×50ms via QSPI)
- Frame dispatch to CAN/LoRa/ZigBee/RS485
- Config query response

**mcu_wan_handler_uplink.c** (Priority 5):
- Handshake with WAN MCU
- Process uplink queue (50 items)
- QSPI transmission + ACK (200ms timeout)
- RTC request (1s interval)
- SD card retry when online

**storage_handler.c**:
- Thread-safe SD operations
- FIFO queue: /sdcard/QUEUE/PKT_NNNNNNNN.dat
- Batch writes (reduced fsync overhead)
- Power-cycle recovery via directory scan

**wan_comm.c** (QSPI Master):
- QIO mode: SPI_TRANS_MODE_QIO
- 40 MHz, 4-bit parallel
- DMA buffers: 16KB TX/RX
- Full-duplex transactions

### 4.2. WAN MCU

**mcu_lan_handler_uplink.c** (Priority 6):
- QSPI slave receive loop
- Frame dispatch: CF/DT/DQ
- Forward to MQTT/HTTP
- RTC response (PCF8563)

**mcu_lan_handler_downlink.c** (No Task):
- Downlink queue (20 items)
- GPIO8 data-ready signaling
- Config cache management
- Async config request (semaphore-based)

**lan_comm.c** (QSPI Slave):
- QIO mode reception
- DMA buffers: 8KB TX/RX
- TX buffer flush: disable→write→enable
- Hardware-driven QIO

---

## 5. TASK ARCHITECTURE

### 5.1. LAN MCU - Dual Task

**Downlink Poll Task** (Priority 7):

Block on GPIO46 ISR → Acquire g_qspi_mutex → Retry DQ (10×50ms)
→ Parse frame → Dispatch → Release mutex → Block
Total: <5ms ISR response, 500ms max retry

text

**Uplink Handler Task** (Priority 5):

Phase 1: Handshake (blocking until success)
Phase 2: Loop {
Queue check → Build DT packet → QSPI send (0.5ms)
→ Wait ACK (200ms) → On fail: storage_save()
RTC request (1s interval)
SD retry (if online + has_data)
}
Mutex timeout: 50ms (yields gracefully)

text

### 5.2. WAN MCU - Single Task

**Uplink Processor** (Priority 6):

Pre-queue RX → Block on completion (100ms) → Parse frame
→ Dispatch: CF (handshake/RTC) | DT (forward to server) | DQ (check pending)
→ Re-queue transaction

text

**Downlink** (No Task): MQTT calls mcu_lan_enqueue_downlink() → signal GPIO8

---

## 6. COMMUNICATION PROTOCOL

### 6.1. Frame Types

| Header | Value | Direction | QSPI Time (512B) | Purpose |
|--------|-------|-----------|------------------|---------|
| CF | 0x4346 | LAN → WAN | ~0.1 ms | Command |
| DT | 0x4454 | Bidirectional | ~0.5 ms | Data (vs 2ms standard SPI) |
| DQ | 0x4451 | LAN → WAN | ~0.05 ms | Query slave |
| CQ | 0x4351 | Bidirectional | ~0.2 ms | Config |
| RT | ASCII | WAN → LAN | ~0.05 ms | RTC status |
| ACK | 0x02 | WAN → LAN | ~0.05 ms | Acknowledgment |

### 6.2. Key Packets

**Handshake**: `[CF][0x01]` → Response: `[ACK][0x10][internet_flag]`

**Data**: `[DT][handler_type(3)][length(2)][payload]`
- Handler types: CAN, LOR, ZIG, RS4

**RTC**: `[CF][RT]` → Response: `[RT][dd/mm/yyyy-hh:mm:ss][status]`

**Config**: `[CF][CQ]` → Response: `[CQ][length(2)][key=value|key=value|...]`

**ACK Codes**:
- 0x10: Handshake OK
- 0x11: Data received
- 0x12: Internet online
- 0x13: Internet offline

---

## 7. DATA FLOW

### 7.1. Uplink (LAN → WAN → Server)

Handler → mcu_wan_enqueue_uplink() → Queue → Uplink task
→ Build DT → QSPI send (0.5ms) → Wait ACK (200ms)
→ If OK: success | If fail: storage_save()
WAN receives → Parse → server_handler_enqueue_uplink() → MQTT
Total latency: 10-20ms (vs 64-155ms standard SPI)

text

### 7.2. Downlink (Server → WAN → LAN)

MQTT → mcu_lan_enqueue_downlink() → signal_data_ready()
→ GPIO46 ISR → Downlink task wakes → DQ retry (10×50ms)
→ QSPI read (0.5ms) → Dispatch to handler
Total latency: <5ms (vs 50-100ms standard SPI)

text

### 7.3. SD Backup/Recovery

Offline: uplink fails → storage_handler_save() → /sdcard/QUEUE/PKT_*.dat
Online: storage_handler_read_oldest() → QSPI send → ACK OK → delete
QSPI advantage: 50 packets/sec (vs 10 packets/sec standard SPI)

text

---

## 8. PERFORMANCE CHARACTERISTICS

### 8.1. QSPI vs Standard SPI

| Metric | Standard SPI | QSPI | Improvement |
|--------|--------------|------|-------------|
| Clock | 10 MHz | 40 MHz | 4× |
| Data width | 1-bit | 4-bit | 4× |
| Throughput | ~2 MB/s | ~8 MB/s | 4× |
| Uplink latency | 64-155ms | 10-20ms | 6-8× |
| Downlink latency | 50-100ms | <5ms | 10-20× |
| SD recovery rate | 10 pkt/s | 50 pkt/s | 5× |

### 8.2. Timing Parameters

| Parameter | Value | Location | Notes |
|-----------|-------|----------|-------|
| QSPI Clock | 40 MHz | wan_comm.c, lan_comm.c | Mandatory |
| GPIO ISR Response | <5 ms | mcu_wan_handler_downlink.c | ISR → data received |
| DQ Retry Interval | 50 ms | mcu_wan_handler_downlink.c | 10 retries max |
| ACK Wait Timeout | 200 ms | mcu_wan_handler_uplink.c | Reduced from 1000ms |
| RTC Update | 1000 ms | mcu_wan_handler_uplink.c | Periodic |
| QSPI Mutex Timeout | 50 ms | mcu_wan_handler_uplink.c | Non-blocking |

---

## 9. SYNCHRONIZATION

### 9.1. Mutexes

**LAN MCU**:
- `g_qspi_mutex`: Binary semaphore, protects QSPI bus
- `g_storage_mutex`: Protects SD operations
- `g_rtc_mutex`: Protects RTC cache

**WAN MCU**:
- `g_config_mutex`: Protects config cache
- `g_downlink_queue_mutex`: Protects downlink queue

### 9.2. Queues

- LAN uplink: 50 items × 532B = 26 KB
- WAN downlink: 20 items × 1030B = 20 KB

**QSPI Benefit**: Faster drain rate (4× throughput), lower overflow risk

---

## 10. ERROR HANDLING

### 10.1. QSPI Errors

**LAN MCU**:
- DQ retry exhaustion (10×50ms): Log, continue (non-fatal)
- Send failure: Fallback to storage_handler_save()
- ACK timeout (200ms): Assume offline, save to SD
- DMA error: Check buffer alignment, retry

**WAN MCU**:
- Parse error: Log, discard, re-queue
- TX load failure: Log, skip transmission
- Slave overrun: Increase buffer size

### 10.2. SD Card Errors

- Mount failure: Log, continue without backup (non-fatal)
- Write failure: Check directory, auto-delete corrupt files
- Read failure: Delete corrupt file, scan next
- Power cycle: scan_and_update_counters() recovers state

---

## 11. MEMORY USAGE

**LAN MCU**: ~64 KB heap (QSPI buffers 16KB + uplink queue 26KB + storage 4KB + SD cache 4KB) + 8 KB stack

**WAN MCU**: ~33 KB heap (QSPI buffers 8KB + downlink queue 20KB) + 4 KB stack

---

## 12. KEY DESIGN DECISIONS

1. **QSPI Mandatory**: 40 MHz, 4-bit, QIO mode for 4× throughput
2. **Modular Separation**: Uplink/downlink split for clarity and independent scaling
3. **Storage Abstraction**: storage_handler.c wraps SDCard_comm with thread safety
4. **Priority-Based Preemption**: Downlink (7) > Uplink (5) for <5ms ISR response
5. **Reduced Timeouts**: ACK 200ms (vs 1s), DQ retry 50ms (vs 150ms) due to QSPI speed
6. **SD FIFO Queue**: Sequential files for power-cycle recovery
7. **Async Config Request**: Heap-allocated + semaphore for timeout handling
8. **No ACK on Downlink**: GPIO ISR + DQ polling (stateless, high speed)

---

## 13. MIGRATION FROM STANDARD SPI

**Hardware**: Change SPI pins to GPIO10-15 (QSPI), keep SD on GPIO3-8

**Software**:
- wan_comm.c: Add `SPI_TRANS_MODE_QIO`, increase clock to 40 MHz
- lan_comm.c: Configure slave for QIO reception
- Reduce timeouts: ACK 200ms, DQ retry 50ms
- Increase DMA buffers: 16KB (master), 8KB (slave)

**Testing**: Verify 4× throughput improvement, <5ms ISR latency

---