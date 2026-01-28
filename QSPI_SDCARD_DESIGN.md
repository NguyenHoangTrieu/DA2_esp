# ARCHITECTURE DESIGN: QSPI REFACTORING & SD CARD OPTIMIZATION
---

## 1. KIẾN TRÚC TỔNG QUAN

### 1.1. Layered Architecture
```

┌─────────────────────────────────────────────────────────────┐
│                    APPLICATION LAYER                         │
│  ┌──────────────────────┐      ┌──────────────────────┐    │
│  │   WAN MCU (ESP32)    │      │   LAN MCU (ESP32)    │    │
│  │  Stream TX/RX Tasks  │◄────►│  Stream TX/RX Tasks  │    │
│  │  (Slave HD Mode)     │ QSPI │  (Master Mode)       │    │
│  └──────────────────────┘      └──────────────────────┘    │
└──────────────────┬──────────────────────────┬───────────────┘
│                          │
┌──────────────────▼──────────────────────────▼───────────────┐
│                      BSP LAYER (SYMMETRIC)                   │
│  ┌────────────────────────────────────────────────────┐     │
│  │  qspi_hal.c - Dual-buffer DMA streaming           │     │
│  │  -  Master: spi_master.h (QIO mode)                │     │
│  │  -  Slave:  spi_slave_hd.h (QIO mode)              │     │
│  └────────────────────────────────────────────────────┘     │
└──────────────────────────────┬───────────────────────────────┘
│
┌──────────────────────────────▼───────────────────────────────┐
│                       HARDWARE LAYER                          │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  QSPI Bus (4-bit, 40MHz, DMA-enabled)              │     │
│  │  GPIO: CS, CLK, D0-D3, DR_WAN, DR_LAN              │     │
│  └─────────────────────────────────────────────────────┘     │
└───────────────────────────────────────────────────────────────┘

```

### 1.2. Key Design Principles

| Principle | Implementation | Benefit |
|-----------|----------------|---------|
| **Zero-ACK** | Transaction-based reliability | No wait time |
| **Dual-buffer** | Ping-pong DMA buffers | Continuous streaming |
| **Ring buffer** | 64KB circular buffers | Smooth data flow |
| **Batch write** | 100KB SD card writes | Minimal fsync() calls |
| **Stream mode** | No packet boundaries | Max throughput |
| **GPIO handshake** | Data-ready signaling only | Minimal overhead |

### 1.3. Performance Comparison

| Metric | Old (Standard SPI) | New (QSPI Stream) | Improvement |
|--------|-------------------|-------------------|-------------|
| **Bandwidth** | ~2 MB/s | ~8 MB/s | 4x |
| **Latency** | 50-100ms (ACK wait) | <5ms (stream) | 10-20x |
| **CPU usage** | 15% (polling) | <3% (DMA) | 5x |
| **SD writes/sec** | 100 (per packet) | 1 (batch) | 100x fewer |

---

## 2. BSP LAYER - QSPI HARDWARE ABSTRACTION

### 2.1. File Structure

```

BSP/
├── QSPI_Driver/
│   ├── include/
│   │   ├── qspi_hal.h              \# Public API
│   │   └── qspi_hal_config.h       \# Hardware config
│   └── src/
│       ├── qspi_hal_master.c       \# LAN MCU (master)
│       ├── qspi_hal_slave.c        \# WAN MCU (slave HD)
│       └── qspi_hal_common.c       \# Shared utilities
│
└── SDCard_Driver/
├── include/
│   └── sdcard_hal.h
└── src/
└── sdcard_hal.c            \# SDMMC init + FAT

```

---

## 3. APPLICATION LAYER - STREAM PROTOCOL

### 3.1. File Structure

```
Application/
├── MCU_LAN_Handler/                    # WAN MCU (Slave)
│   ├── include/
│   │   ├── mcu_lan_stream.h
│   │   └── mcu_lan_protocol.h
│   └── src/
│       ├── mcu_lan_stream.c            # Main init
│       ├── mcu_lan_tx_task.c           # Uplink to LAN
│       └── mcu_lan_rx_task.c           # Downlink from LAN
│
├── MCU_WAN_Handler/                    # LAN MCU (Master)
│   ├── include/
│   │   ├── mcu_wan_stream.h
│   │   └── mcu_wan_protocol.h
│   └── src/
│       ├── mcu_wan_stream.c            # Main init
│       ├── mcu_wan_tx_task.c           # Uplink to WAN
│       └── mcu_wan_rx_task.c           # Downlink from WAN
│
└── Storage_Handler/
    ├── include/
    │   └── storage_handler.h
    └── src/
        └── storage_handler.c           # Ring buffer + SD batch
```

---

## 6. PERFORMANCE ANALYSIS

### 6.1. Latency Breakdown

| Stage | Old (Standard SPI + ACK) | New (QSPI Stream) | Improvement |
| :-- | :-- | :-- | :-- |
| **Pack frame** | 0.5 ms | 0.5 ms | Same |
| **Wait peer ready** | 10-50 ms | 0 ms (pre-queued) | ∞ |
| **TX transaction** | 2 ms (1-bit @ 10MHz) | 0.5 ms (4-bit @ 40MHz) | 4x |
| **Wait ACK** | 50-100 ms | 0 ms (no ACK) | ∞ |
| **RX ACK transaction** | 2 ms | 0 ms | ∞ |
| **Total per packet** | 64-155 ms | **1 ms** | **64-155x** |

### 6.2. Throughput

```
Old: ~100 packets/sec × 100 bytes = 10 KB/s
New: ~8000 packets/sec × 100 bytes = 800 KB/s (80x improvement)

Maximum QSPI throughput: 40 MHz × 4 bits = 20 MB/s (theoretical)
Actual: ~8 MB/s (accounting for protocol overhead)
```


### 6.3. SD Card Write Optimization

```
Old: 100 writes/sec × 5ms fsync() = 500 ms/sec spent in I/O
New: 1 write/sec × 5ms fsync() = 5 ms/sec spent in I/O (100x reduction)
```


### 6.4. Memory Usage

```
QSPI buffers: 4KB × 2 × 2 = 16 KB (dual-buffer × TX/RX × 2 MCUs)
Ring buffers: 64KB × 2 (TX/RX per MCU) = 128 KB
SD ring buffer: 100 KB
Total: ~250 KB (acceptable for ESP32-S3)
```


---

## 7. CONFIGURATION EXAMPLE

### 7.1. menuconfig Options

```kconfig
menu "QSPI Configuration"
    config QSPI_FREQ_MHZ
        int "QSPI Frequency (MHz)"
        default 40
        range 10 80
        
    config QSPI_BUFFER_SIZE
        int "DMA Buffer Size (bytes)"
        default 4096
        
    config QSPI_ENABLE_STATS
        bool "Enable statistics"
        default y
endmenu

menu "Storage Configuration"
    config STORAGE_RING_BUFFER_SIZE
        int "Ring buffer size (KB)"
        default 100
        
    config STORAGE_FLUSH_THRESHOLD
        int "Flush threshold (KB)"
        default 80
endmenu
```


### 7.2. GPIO Pin Mapping

```
ESP32-S3 QSPI Pins (Both MCUs):
  CLK:  GPIO12
  CS:   GPIO10
  D0:   GPIO11 (MOSI equivalent)
  D1:   GPIO13 (MISO equivalent)
  D2:   GPIO14 (WP)
  D3:   GPIO15 (HOLD)
  DR:   GPIO46 (Data Ready handshake on WAN MCU), GPIO 8 (on LAN MCU)
```


---

## 7. SYSTEM FLOWCHARTS

### 7.1. Uplink Task Flow (LAN MCU → WAN MCU)

```mermaid
flowchart TD
    Start([Uplink Task Start<br/>LAN MCU]) --> InitBuf[Init 64KB Ring Buffer]
    InitBuf --> WaitQueue[xQueueReceive<br/>uplink_queue]
    
    WaitQueue --> CheckData{Data<br/>received?}
    CheckData -->|Timeout 100ms| CheckFlush{Buffer > 0?}
    CheckFlush -->|Yes| Flush
    CheckFlush -->|No| WaitQueue
    
    CheckData -->|Yes| PackFrame[Pack frame:<br/>SYNC+TYPE+LEN+PAYLOAD+CRC]
    PackFrame --> AppendRing[Append to ring buffer]
    
    AppendRing --> CheckThresh{Buffer ><br/>4KB?}
    CheckThresh -->|No| WaitQueue
    CheckThresh -->|Yes| Flush[qspi_hal_stream_write<br/>DMA transfer 4-bit]
    
    Flush --> ResetBuf[Reset buffer index]
    ResetBuf --> WaitQueue
```


### 7.2. Downlink Task Flow (WAN MCU → LAN MCU)

```mermaid
flowchart TD
    ```
    Start([Downlink Task Start<br/>WAN MCU]) --> RegCB[Register RX callback<br/>with QSPI HAL]
    ```
    RegCB --> WaitCB[Wait for callback trigger<br/>from DMA ISR]
    
    WaitCB --> CBTrigger{Callback<br/>invoked?}
    CBTrigger -->|Yes| ParseFrame[Parse frame:<br/>extract TYPE+PAYLOAD]
    
    ParseFrame --> CheckType{Frame<br/>Type?}
    
    CheckType -->|DATA| DispatchHandler[Dispatch to handler<br/>CAN/LORA/ZIGBEE/RS485]
    CheckType -->|CONFIG| UpdateConfig[Update config cache]
    CheckType -->|RTC| SendRTC[Send RTC response<br/>to LAN MCU]
    CheckType -->|STATUS| UpdateInternet[Update internet status]
    CheckType -->|FOTA_NOTIFY| LogFOTA[Log FOTA available]
    
    DispatchHandler --> WaitCB
    UpdateConfig --> WaitCB
    SendRTC --> WaitCB
    UpdateInternet --> WaitCB
    LogFOTA --> WaitCB
```


### 7.3. SD Storage Handler Flow

```mermaid
flowchart TD
    Start([Storage Task Start]) --> InitSD[Init SD card SDMMC<br/>Mount FAT32]
    InitSD --> InitRing[Alloc 100KB ring buffer<br/>in heap]
    
    InitRing --> Loop[Every 1 second check]
    Loop --> CheckThresh{Buffer ><br/>80KB?}
    
    CheckThresh -->|No| Loop
    CheckThresh -->|Yes| OpenFile[fopen /sdcard/data.log<br/>append mode]
    
    OpenFile --> WriteAll[fwrite entire buffer<br/>1 chunk 100KB]
    WriteAll --> Sync[fflush + fsync<br/>1 time only]
    
    Sync --> Close[fclose]
    Close --> Reset[Reset buffer:<br/>write_idx = 0]
    Reset --> Loop
```


### 7.4. QSPI HAL Stream Task (Master Side - LAN MCU)

```mermaid
flowchart TD
    Start([QSPI Master<br/>Stream Task<br/>LAN MCU]) --> CheckTX{TX queue<br/>has data?}
    
    CheckTX -->|Yes| CopyDMA[Copy to DMA buffer<br/>ping-pong buffer 0 or 1]
    CopyDMA --> AssertDR[Assert DR GPIO]
    AssertDR --> TXTrans[spi_device_transmit<br/>CMD=0xAA WRITE QIO]
    TXTrans --> DeassertDR[Deassert DR GPIO]
    DeassertDR --> ToggleBuf[Toggle buffer index]
    
    CheckTX -->|No| RXPoll[Poll RX:<br/>spi_device_transmit<br/>CMD=0x55 READ QIO]
    
    ToggleBuf --> RXPoll
    RXPoll --> CheckRX{RX data<br/>valid?}
    
    CheckRX -->|Yes| InvokeCB[Invoke RX callback]
    CheckRX -->|No| CheckTX
    InvokeCB --> CheckTX
```


### 7.5. QSPI HAL RX Task (Slave Side - WAN MCU)

```mermaid
flowchart TD
    ```
    Start([QSPI Slave<br/>RX Task<br/>WAN MCU]) --> PreQueue[Pre-queue RX buffers<br/>spi_slave_hd_queue_trans<br/>CHAN_RX]
    ```
    
    PreQueue --> WaitTrans[spi_slave_hd_get_trans_res<br/>Block until master writes]
    
    WaitTrans --> CheckRes{Trans<br/>complete?}
    CheckRes -->|Timeout| WaitTrans
    
    CheckRes -->|Success| InvokeCB[Invoke RX callback<br/>with received data]
    InvokeCB --> ReQueue[Re-queue RX buffer<br/>for next transaction]
    
    ReQueue --> WaitTrans
```


---

## 8. SEQUENCE DIAGRAMS

### 8.1. Complete Communication Flow

```mermaid
sequenceDiagram
    participant H as Handler<br/>(CAN/LORA)
    participant LT as LAN MCU<br/>Uplink Task
    participant QM as QSPI Master<br/>(LAN MCU)
    participant QS as QSPI Slave<br/>(WAN MCU)
    participant WT as WAN MCU<br/>RX Task
    participant SRV as Server<br/>(MQTT/HTTP)
    
    Note over H,SRV: Uplink Data Flow (LAN → WAN → Server)
    
    H->>LT: Enqueue sensor data
    LT->>LT: Pack frame (SYNC+TYPE+LEN+DATA+CRC)
    LT->>LT: Append to 64KB ring buffer
    
    alt Buffer > 4KB
        LT->>QM: qspi_hal_stream_write(buffer, len)
        QM->>QM: Assert DR GPIO (notify slave)
        QM->>QS: WRITE transaction (CMD=0xAA, 4-bit QIO)
        QS->>QS: RX callback triggered
        QS->>WT: Parse frame, forward to server
        WT->>SRV: MQTT/HTTP publish
        QM->>QM: Deassert DR GPIO
    end
    
    Note over H,SRV: Downlink Data Flow (Server → WAN → LAN)
    
    SRV->>WT: Downlink command (RTC/Config/Data)
    WT->>QS: qspi_hal_stream_write(frame)
    QS->>QS: Pre-queue TX buffer (spi_slave_hd_queue_trans)
    
    QM->>QM: Periodic READ transaction (CMD=0x55)
    QM-->>QS: SPI transaction (4-bit QIO)
    QS-->>QM: DMA transfer data
    
    QM->>LT: RX callback with data
    LT->>LT: Parse frame, validate CRC
    LT->>H: Dispatch to handler (CAN/LORA/etc)
```


### 8.2. Simplified Uplink Flow (LAN → WAN)

```mermaid
sequenceDiagram
    participant LAN as LAN MCU<br/>Uplink Task<br/>(Sensor side)
    participant QSPI as QSPI Bus<br/>(40MHz 4-bit)
    participant WAN as WAN MCU<br/>Stream Task<br/>(Internet side)
    
    Note over LAN,WAN: Continuous Streaming (No ACK)
    
    loop Every 100ms or buffer full
        LAN->>LAN: Accumulate sensor data in ring buffer
        LAN->>QSPI: DMA write (4KB chunk)
        Note right of LAN: <1ms transfer time
        
        QSPI->>WAN: Data ready notification
        WAN->>WAN: RX callback processes frames
        
        par Parallel operations
            WAN->>WAN: Forward to MQTT/HTTP server
        and
            LAN->>LAN: Continue accumulating next chunk
        end
    end
    
    Note over LAN,WAN: Total latency: <5ms (vs 64-155ms with ACK)
```


### 8.3. Downlink Flow (WAN → LAN) - RTC Example

```mermaid
sequenceDiagram
    participant LAN as LAN MCU
    participant QSPI as QSPI Bus
    participant WAN as WAN MCU
    participant RTC as RTC Module<br/>(PCF8563)
    
    Note over LAN,WAN: RTC Request (Downlink from WAN)
    
    LAN->>LAN: Timer expires (1 second)
    LAN->>QSPI: Send RTC_REQUEST frame (uplink)
    QSPI->>WAN: Master WRITE
    
    WAN->>RTC: pcf8563_read_time()
    RTC-->>WAN: Return timestamp
    
    WAN->>WAN: Pack RTC_RESPONSE frame
    WAN->>QSPI: Pre-queue TX (spi_slave_hd_queue_trans)
    
    LAN->>QSPI: Periodic READ poll (CMD=0x55)
    QSPI-->>LAN: Return RTC frame (downlink)
    
    LAN->>LAN: Update RTC cache
    
    Note over LAN,WAN: Downlink = WAN sends data to LAN
```


### 8.4. SD Card Batch Write Sequence

```mermaid
sequenceDiagram
    participant APP as Application<br/>Tasks
    participant RING as Ring Buffer<br/>(100KB RAM)
    participant SD as Storage Task
    participant FS as SD Card<br/>FAT32
    
    Note over APP,FS: Continuous Data Accumulation
    
    loop Every packet
        APP->>RING: Append data (lock mutex)
        RING-->>APP: Success
    end
    
    Note over APP,FS: Periodic Flush (80KB threshold)
    
    SD->>SD: Check every 1 second
    
    alt Buffer > 80KB
        SD->>RING: Lock mutex
        SD->>FS: fopen("/sdcard/data.log", "ab")
        SD->>FS: fwrite(buffer, 100KB)
        Note right of SD: Single write, no chunking
        
        SD->>FS: fflush()
        SD->>FS: fsync()
        Note right of SD: Sync once per batch
        
        SD->>FS: fclose()
        SD->>RING: Reset buffer, unlock mutex
    end
    
    Note over APP,FS: 100x fewer fsync() calls vs old design
```


### 8.5. GPIO Handshake Timing

```mermaid
sequenceDiagram
    participant M as Master<br/>(LAN MCU)
    participant DR as DR GPIO<br/>Line
    participant S as Slave<br/>(WAN MCU)
    
    Note over M,S: Uplink: LAN signals data ready
    
    M->>M: Prepare TX data in buffer
    M->>DR: Assert HIGH
    Note right of DR: Signal: "Data ready to send"
    
    M->>S: WRITE transaction (CMD=0xAA)
    Note over M,S: 4-bit QIO DMA transfer
    
    S-->>M: DMA complete (no ACK frame)
    M->>DR: Deassert LOW
    
    Note over M,S: Total overhead: <100μs
```


### 8.6. Error Recovery Flow

```mermaid
sequenceDiagram
    participant LAN as LAN MCU<br/>(Uplink sender)
    participant QSPI as QSPI HAL
    participant WAN as WAN MCU<br/>(Receiver)
    
    Note over LAN,WAN: CRC Error Detection
    
    LAN->>QSPI: Stream write data
    QSPI->>WAN: DMA transfer
    
    WAN->>WAN: Validate CRC16
    
    alt CRC Valid
        WAN->>WAN: Process frame normally
    else CRC Invalid
        WAN->>WAN: Drop frame silently
        Note right of WAN: No NACK sent (stateless)
        WAN->>WAN: Log error stats
    end
    
    Note over LAN,WAN: Upper layer handles retransmission if needed
    
    alt DMA Error
        QSPI->>QSPI: Increment error counter
        QSPI->>LAN: Return ESP_FAIL
        LAN->>LAN: Retry from ring buffer
    end
```


### 8.7. Bidirectional Communication Summary

```mermaid
sequenceDiagram
    participant Sensors as Sensor Handlers<br/>(CAN/LORA/RS485)
    participant LAN as LAN MCU<br/>(Master)
    participant WAN as WAN MCU<br/>(Slave)
    participant Server as Internet Server<br/>(MQTT/HTTP)
    
    rect rgba(0, 0, 0, 1)
        Note over Sensors,Server: UPLINK: LAN → WAN → Server
        Sensors->>LAN: Sensor data
        LAN->>WAN: QSPI WRITE (4-bit)
        WAN->>Server: Publish telemetry
    end
    
    rect rgba(0, 0, 0, 1)
        Note over Sensors,Server: DOWNLINK: Server → WAN → LAN
        Server->>WAN: Commands/Config
        WAN->>LAN: QSPI READ by master (4-bit)
        LAN->>Sensors: Execute command
    end
    
    rect rgba(0, 0, 0, 1)
        Note over Sensors,Server: DOWNLINK: RTC Sync (WAN → LAN)
        LAN->>WAN: Request RTC
        WAN->>WAN: Read PCF8563
        WAN->>LAN: Return timestamp
        LAN->>Sensors: Update cached time
    end
```


---

## 9. PERFORMANCE METRICS

### 9.1. Latency Comparison

```
Old Design (Standard SPI + ACK):
┌─────────┬─────────┬──────────┬─────────┬─────────┐
│ Pack    │ GPIO    │ TX 1-bit │ Wait    │ RX ACK  │
│ Frame   │ Wait    │ @10MHz   │ ACK     │ 1-bit   │
│ 0.5ms   │ 10-50ms │ 2ms      │ 50-100ms│ 2ms     │
└─────────┴─────────┴──────────┴─────────┴─────────┘
Total: 64-155ms per packet

New Design (QSPI Stream):
┌─────────┬─────────┬──────────┐
│ Pack    │ TX 4-bit│ DMA      │
│ Frame   │ @40MHz  │ Done     │
│ 0.5ms   │ 0.5ms   │ instant  │
└─────────┴─────────┴──────────┘
Total: 1ms per packet (64-155x faster)
```


### 9.2. Resource Usage

| Resource | Usage | Notes |
| :-- | :-- | :-- |
| **RAM** | 250 KB | QSPI buffers + ring buffers |
| **CPU (idle)** | <1% | DMA-driven |
| **CPU (peak)** | 3% | During batch flush |
| **SPI bandwidth** | 8 MB/s | 40MHz × 4 bits × 0.5 efficiency |
| **SD write rate** | 1/sec | vs 100/sec old design |

### 9.3. Direction Definitions

| Direction | From | To | Example Use Cases |
| :-- | :-- | :-- | :-- |
| **Uplink** | LAN MCU | WAN MCU | Sensor data, status reports, telemetry |
| **Downlink** | WAN MCU | LAN MCU | RTC sync, config updates, commands, FOTA notify |

**Key Points:**

- LAN MCU = Sensor handlers (CAN/LORA/Zigbee/RS485) - **data source**
- WAN MCU = Internet gateway (WiFi/MQTT/HTTP) - **data sink \& command source**
- Uplink = Data flowing from sensors to internet
- Downlink = Data/commands flowing from internet to sensors

---
