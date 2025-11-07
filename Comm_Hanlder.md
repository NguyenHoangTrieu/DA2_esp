
### MCU WAN Comm Handler Task
```mermaid
stateDiagram-v2
    [*] --> INIT_SLAVE

    INIT_SLAVE --> SETUP_QSPI_SLAVE: Initialize QSPI Slave interface
    SETUP_QSPI_SLAVE --> SETUP_BUFFER_POOL: Setup double buffer pool for RX
    SETUP_BUFFER_POOL --> SETUP_TX_BUFFER: Setup TX buffer for responses
    SETUP_TX_BUFFER --> START_PROCESSING_TASK: Create processing task
    START_PROCESSING_TASK --> IDLE: Ready to receive from WAN master
    START_PROCESSING_TASK --> ERROR: Init failed
    
    ERROR --> IDLE: Retry after timeout
    ERROR --> [*]: Fatal error
    
    IDLE --> PREPARE_NEXT_RX: Prepare for next QSPI transaction
    
    PREPARE_NEXT_RX --> SETUP_RX_DMA: Setup RX DMA with double buffer
    SETUP_RX_DMA --> START_RX_DMA: Activate RX DMA listening
    START_RX_DMA --> WAIT_MASTER_REQUEST: Waiting for master QSPI read/write
    
    WAIT_MASTER_REQUEST --> MASTER_INITIATED_TX_CHECK: Master initiates transaction check
    MASTER_INITIATED_TX_CHECK --> MASTER_WRITING: Master writing data to LAN slave
    MASTER_INITIATED_TX_CHECK --> MASTER_READING: Master reading from LAN slave
    
    MASTER_WRITING --> RX_DMA_ACTIVE: RX DMA active
    RX_DMA_ACTIVE --> DATA_TRANSFER_IN_PROGRESS: Data flowing on QSPI bus
    DATA_TRANSFER_IN_PROGRESS --> RX_DMA_COMPLETE: RX DMA transfer complete ISR
    
    RX_DMA_COMPLETE --> ENQUEUE_TO_QUEUE: Enqueue received buffer to processing queue
    ENQUEUE_TO_QUEUE --> SIGNAL_PROCESSING_TASK: Signal processing task via trans_complete_queue
    SIGNAL_PROCESSING_TASK --> SWAP_RX_BUFFERS: Swap RX buffers for next transfer
    SWAP_RX_BUFFERS --> RESTART_RX_DMA: Restart RX DMA on new buffer
    RESTART_RX_DMA --> NOTIFY_TASK: Notify processing task
    
    NOTIFY_TASK --> PROCESSING_TASK_RUNNING: Processing task awakened
    PROCESSING_TASK_RUNNING --> LOCK_BUFFER: Lock processing buffer
    LOCK_BUFFER --> PARSE_HEADER: Parse 2-byte header from buffer
    
    PARSE_HEADER --> HEADER_VALID_CHECK: Header valid check
    HEADER_VALID_CHECK --> INVALID_HEADER: Invalid header
    HEADER_VALID_CHECK --> VALID_HEADER: Header valid
    
    INVALID_HEADER --> ERROR_CALLBACK: Call error_cb
    ERROR_CALLBACK --> UNLOCK_BUFFER: Unlock processing buffer
    UNLOCK_BUFFER --> WAIT_MASTER_REQUEST: Continue waiting
    
    VALID_HEADER --> EXTRACT_HEADER_VALUE: Extract header value
    EXTRACT_HEADER_VALUE --> HEADER_IS_CF_CHECK: Header equals CF 0x4346 check
    
    HEADER_IS_CF_CHECK --> IS_COMMAND_PACKET: Yes command packet
    HEADER_IS_CF_CHECK --> CHECK_DT_HEADER: No check DT
    
    IS_COMMAND_PACKET --> EXTRACT_COMMAND_DATA: Extract command payload
    EXTRACT_COMMAND_DATA --> DISPATCH_COMMAND_CB: Call on_command_received_cb
    DISPATCH_COMMAND_CB --> PREPARE_ACK_RESPONSE: Prepare ACK response
    PREPARE_ACK_RESPONSE --> LOAD_ACK_TO_TX: Load ACK to TX buffer
    LOAD_ACK_TO_TX --> UNLOCK_BUFFER_CMD: Unlock buffer
    UNLOCK_BUFFER_CMD --> WAIT_MASTER_REQUEST: Continue waiting
    
    CHECK_DT_HEADER --> HEADER_IS_DT_CHECK: Header equals DT 0x4454 check
    HEADER_IS_DT_CHECK --> IS_DATA_PACKET: Yes data packet
    HEADER_IS_DT_CHECK --> UNKNOWN_HEADER: No unknown header
    
    IS_DATA_PACKET --> EXTRACT_DATA_PAYLOAD: Extract data payload
    EXTRACT_DATA_PAYLOAD --> VALIDATE_TIMESTAMP: Check for Timestamp field
    VALIDATE_TIMESTAMP --> TIMESTAMP_PRESENT_CHECK: Timestamp present check
    
    TIMESTAMP_PRESENT_CHECK --> VALID_TIMESTAMP: Timestamp present
    TIMESTAMP_PRESENT_CHECK --> MISSING_TIMESTAMP: Timestamp missing
    
    MISSING_TIMESTAMP --> ERROR_CALLBACK
    
    VALID_TIMESTAMP --> DISPATCH_DATA_CB: Call on_data_received_cb
    DISPATCH_DATA_CB --> PREPARE_DATA_ACK: Prepare ACK packet
    PREPARE_DATA_ACK --> LOAD_DATA_ACK_TO_TX: Load data ACK to TX buffer
    LOAD_DATA_ACK_TO_TX --> UNLOCK_BUFFER_DATA: Unlock buffer
    UNLOCK_BUFFER_DATA --> WAIT_MASTER_REQUEST: Continue waiting
    
    UNKNOWN_HEADER --> ERROR_CALLBACK
    
    MASTER_READING --> TX_DATA_AVAILABLE_CHECK: Check TX buffer has data
    TX_DATA_AVAILABLE_CHECK --> TX_BUFFER_READY: Yes TX buffer ready with ACK RTC or CMD response
    TX_DATA_AVAILABLE_CHECK --> TX_BUFFER_EMPTY: No TX buffer empty
    
    TX_BUFFER_READY --> TX_DMA_ACTIVE: TX DMA active
    TX_DMA_ACTIVE --> SENDING_RESPONSE: Sending response data on QSPI
    SENDING_RESPONSE --> TX_DMA_COMPLETE: TX DMA complete
    TX_DMA_COMPLETE --> WAIT_MASTER_REQUEST: Continue waiting
    
    TX_BUFFER_EMPTY --> SEND_EMPTY_RESPONSE: Send empty or zero response
    SEND_EMPTY_RESPONSE --> WAIT_MASTER_REQUEST: Continue waiting
    
    IDLE --> RTC_UPDATE_BACKGROUND: Background 1 sec timer tick
    RTC_UPDATE_BACKGROUND --> READ_SYSTEM_RTC: Read RTC time from system
    READ_SYSTEM_RTC --> UPDATE_RTC_REGISTER: Update RTC in persistent register
    UPDATE_RTC_REGISTER --> IDLE: RTC ready for master request
```
### MCU LAN Comm Handler Task
```mermaid
stateDiagram-v2
    [*] --> INIT

    INIT --> INIT_QSPI_MASTER: Initialize QSPI Master config
    INIT_QSPI_MASTER --> SETUP_RTC: Setup RTC system
    SETUP_RTC --> START_PERIODIC_TASK: Start periodic polling task
    START_PERIODIC_TASK --> IDLE: Ready to pull data from LAN
    START_PERIODIC_TASK --> ERROR: Init failed
    
    ERROR --> IDLE: Retry after timeout
    ERROR --> [*]: Fatal error
    
    IDLE --> PERIODIC_POLL_TICK: 100ms tick
    
    PERIODIC_POLL_TICK --> ACQUIRE_TRANSFER_MUTEX: Acquire transfer_mutex
    ACQUIRE_TRANSFER_MUTEX --> MUTEX_CHECK: Mutex acquired check
    MUTEX_CHECK --> IDLE: Timeout
    MUTEX_CHECK --> REQUEST_LAN_DATA: Mutex held
    
    REQUEST_LAN_DATA --> SEND_READ_REQUEST: Send read command to LAN slave
    SEND_READ_REQUEST --> WAIT_DMA_TRANSFER: Wait for DMA to complete
    WAIT_DMA_TRANSFER --> DMA_CHECK: DMA done check
    DMA_CHECK --> IDLE: DMA failed
    DMA_CHECK --> DMA_COMPLETE: DMA success
    
    DMA_COMPLETE --> RELEASE_MUTEX_DMA: Release transfer_mutex
    RELEASE_MUTEX_DMA --> PARSE_RECEIVED_DATA: Parse received data from LAN
    
    PARSE_RECEIVED_DATA --> HEADER_TYPE_CHECK: Check packet header type
    HEADER_TYPE_CHECK --> IS_DATA_PACKET: Check if DT packet
    HEADER_TYPE_CHECK --> IS_ACK_PACKET: Check if ACK packet
    HEADER_TYPE_CHECK --> IS_ERROR: Other packet type
    
    IS_DATA_PACKET --> EXTRACT_DATA_PAYLOAD: Extract data payload with Node ID and Timestamp
    EXTRACT_DATA_PAYLOAD --> STORE_DATA_BUFFER: Store in data_buffer
    STORE_DATA_BUFFER --> LOG_DATA_RECEIVED: Log reception
    LOG_DATA_RECEIVED --> IDLE
    
    IS_ACK_PACKET --> PROCESS_ACK: Process ACK packet
    PROCESS_ACK --> UPDATE_ACK_STATUS: Update last ACK status
    UPDATE_ACK_STATUS --> IDLE
    
    IS_ERROR --> LOG_ERROR: Log error packet
    LOG_ERROR --> IDLE
    
    IDLE --> RTC_SYNC_TICK: 1 sec tick
    RTC_SYNC_TICK --> UPDATE_RTC_SYSTEM: Read internal RTC time
    UPDATE_RTC_SYSTEM --> RTC_READY: RTC time ready to serve
    RTC_READY --> IDLE: RTC synced and ready for LAN request
    
    IDLE --> SERVER_COMM_CHECK: Check server connection status
    SERVER_COMM_CHECK --> SERVER_CONNECTED: Server connected check
    SERVER_CONNECTED --> SEND_BUFFERED_DATA: Send data to server
    SEND_BUFFERED_DATA --> IDLE: Data sent to server
    SERVER_CONNECTED --> NO_SERVER_CONNECTION: Server disconnected
    NO_SERVER_CONNECTION --> BUFFER_LOCALLY: Buffer data locally
    BUFFER_LOCALLY --> IDLE: Data buffered for later
    
    IDLE --> HANDLE_COMMAND_REQUEST: Command from application layer
    HANDLE_COMMAND_REQUEST --> ACQUIRE_MUTEX_CMD: Acquire transfer_mutex
    ACQUIRE_MUTEX_CMD --> SEND_COMMAND_TO_LAN: Send command to LAN slave
    SEND_COMMAND_TO_LAN --> WAIT_CMD_DMA: Wait for DMA complete
    WAIT_CMD_DMA --> RELEASE_MUTEX_CMD: Release transfer_mutex
    RELEASE_MUTEX_CMD --> IDLE: Command sent
```