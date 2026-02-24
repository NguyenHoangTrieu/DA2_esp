# Công việc sửa firmware chính cho giai đoạn 2.1
## Tối ưu hóa sử dụng năng lượng phù hợp cho sleep mode và tiết kiệm năng lượng (mục tiêu quan trọng)
- tối ưu hóa code để giảm tiêu thụ năng lượng trong các chế độ sleep mode cho baseboard và gateway.
## Sửa lại đường truyền spi giữa lan và wan, cập nhật thành Quad SPI,
- thay đổi chia làm 2 task riêng biệt cho 2 đường truyền chính, một task truyền uplink và một task truyền downlink

- cập nhật BSP và File Handler sao cho đạt được tốc độ tối đa ghi đọc của quad SPI (có thể thêm các file nếu cần thiết)

- Uu tiên sử dụng DMA để truyền dữ liệu giữa lan và wan

- gpio trigger interrupt sẽ ở trong cái file thuộc BSP của giao tiếp SPI, kg nằm trong file application nữa

- giảm tối đa các delay giữa truyền nhận, giữa các gói ack và data sao cho dữ liệu truyền đi càng liền mạch, càng nhanh càng tốt

- vẽ lại (dùng mermaid) flow chart và sequence diagram cho việc truyền nhận dữ liệu giữa lan và wan qua giao tiếp Quad SPI
### YÊU CẦU KỸ THUẬT: NÂNG CẤP GIAO TIẾP LAN-WAN (QSPI REFACTORING)
Mục tiêu dự án: Tối ưu hóa băng thông và độ trễ giao tiếp giữa module LAN và WAN bằng cách chuyển đổi từ Standard SPI sang Quad SPI (QSPI), tái cấu trúc luồng xử lý (Task/BSP) để đạt hiệu suất cao nhất.

1. Tầng Driver & BSP (Board Support Package)
Mục tiêu: Đảm bảo phần cứng hoạt động ở chế độ nhanh nhất và code phần cứng được tách biệt hoàn toàn khỏi logic ứng dụng.

1.1. Nâng cấp giao thức vật lý (PHY):

Chuyển đổi cấu hình SPI Bus từ chế độ 1-bit (Standard) sang 4-bit (Quad SPI).

Bắt buộc sử dụng DMA (Direct Memory Access) cho cả chiều truyền (TX) và nhận (RX) để giải phóng CPU.

Tăng tần số xung nhịp (Clock Speed) lên mức tối đa mà phần cứng cho phép và ổn định.

1.2. Tái cấu trúc xử lý ngắt (Interrupt Handling):

Yêu cầu: Di chuyển toàn bộ logic khởi tạo ngắt (ISR setup) và xử lý ngắt GPIO (GPIO Trigger) từ lớp Application vào trong lớp BSP/Driver.

Cơ chế: Khi có tín hiệu ngắt từ phần cứng, BSP sẽ xử lý sơ bộ (clear flag), sau đó báo hiệu cho lớp trên thông qua cơ chế Callback hoặc Semaphore/Event Group, tuyệt đối không xử lý logic nghiệp vụ trong ISR.

1.3. File Handler & Buffer Management:

Tạo hoặc cập nhật các file xử lý dữ liệu (ví dụ: qspi_driver.c, qspi_utils.c) để quản lý việc đóng gói dữ liệu cho chế độ QSPI (QIO/QOUT).

Tối ưu hóa bộ đệm (Buffer alignment) để tương thích tốt nhất với DMA (thường yêu cầu align 4-byte).

2. Tầng Ứng dụng & RTOS (Task Management)
Mục tiêu: Xử lý song song và giảm thời gian chờ đợi.

2.1. Phân tách tác vụ (Task Splitting):

Thay vì dùng 1 vòng lặp lớn, hãy chia logic thành 2 RTOS Tasks riêng biệt:

Task Uplink (LAN -> WAN): Chịu trách nhiệm lấy dữ liệu từ hàng đợi (Queue) và đẩy xuống QSPI qua DMA.

Task Downlink (WAN -> LAN): Chịu trách nhiệm chờ tín hiệu (từ ISR/Semaphore), nhận dữ liệu qua DMA và đẩy vào hàng đợi xử lý.

Lưu ý: Cần cơ chế khóa (Mutex) để điều phối quyền truy cập Bus QSPI vì QSPI là bán song công (Half-duplex).

2.2. Logic truyền nhận:

Loại bỏ các hàm delay() cứng (blocking delay). Thay thế bằng cơ chế chờ sự kiện (Blocking on Semaphore/Queue) để CPU có thể làm việc khác hoặc sleep khi rảnh.

3. Tối ưu hóa hiệu năng (Performance Tuning)
Mục tiêu: "Zero Dead Time" - Giảm thiểu khoảng lặng giữa các gói tin.

3.1. Giảm độ trễ giao thức (Latency Reduction):

Tối giản hóa quy trình Handshake: Nếu có gửi ACK (xác nhận), hãy thực hiện ngay lập tức sau khi DMA báo xong (DMA Complete Interrupt), không chờ task switch nếu không cần thiết.

Gộp dữ liệu (Batching) nếu có thể để tận dụng tối đa lợi thế của DMA (truyền 1 cục to nhanh hơn nhiều cục nhỏ).

4. Mô hình hóa hệ thống (Mermaid Diagrams)
Vẽ lại sơ đồ luồng dữ liệu và trình tự xử lý để bạn hình dung kiến trúc mới.

## Viết lại SD Card Driver để tối ưu hóa tốc độ ghi đọc
- cập nhật lại các file handler của BSP cho SD card để tối ưu tốc độ ghi đọc

- đệm dữ liệu theo khối lớn hơn thay vì từng block nhỏ như cũ trước kia để giảm số lần ghi đọc vật lý trên SD card và gửi dữ liệu lên server nhanh hơn

- nếu internet maatsd kết nối, thay vì đệm dữ liệu liên tục và lưu liên tục vào SD card tì đệm vào ram, cho tới khi đạt ngưỡng khoảng 0.1MB thì mới ghi 1 lần vào SD card

- vẽ lại flow chart và sequence diagram cho việc lưu dữ liệu vào SD card (dùng mermaid)

- Tạo một task khác để xử lý việc ghi dữ liệu từ ram vào SD card, task này sẽ chạy ngầm và ưu tiên thấp hơn task chính truyền dữ liệu lên server qua lan-wan communication (task này nhận queue từ task lan-wan để đệm và ghi dữ liệu vào SD card)

### YÊU CẦU KỸ THUẬT: TỐI ƯU HÓA SD CARD & CHIẾN LƯỢC ĐỆM DỮ LIỆU
Mục tiêu:

Tăng tốc độ I/O: Giảm overhead của filesystem bằng cách ghi theo block lớn.

Kéo dài tuổi thọ SD Card: Giảm hiện tượng "Write Amplification" do ghi quá nhiều file nhỏ lẻ.

Cơ chế Fail-safe: Quản lý RAM thông minh khi mất mạng (Offline mode) với ngưỡng 0.1MB.

1. Cập nhật BSP & Driver (Tầng thấp)
Mục tiêu: Đảm bảo Driver SD Card hoạt động ở hiệu suất cao nhất để không block CPU quá lâu.

1.1. Tăng tần số xung nhịp (Clock Speed):

Cấu hình lại driver SDMMC/SPI để chạy ở tần số cao nhất mà phần cứng hỗ trợ (ví dụ: 20MHz hoặc 40MHz).

Nếu phần cứng hỗ trợ 4-line mode (SDMMC), bắt buộc cấu hình 4-line thay vì 1-line SPI để tăng băng thông gấp 4 lần.

1.2. Tối ưu hóa Allocation Unit (AU) của FATFS:

Trong menuconfig hoặc code khởi tạo FATFS, tăng kích thước Allocation Unit Size (Cluster size) lên (ví dụ: 16KB hoặc 32KB) để phù hợp với việc ghi file lớn, giảm phân mảnh.

1.3. Điều chỉnh bộ đệm File Handler:

Sử dụng hàm setvbuf() cho file stream để tăng kích thước bộ đệm nội bộ của thư viện C chuẩn, khớp với kích thước sector của SD Card (thường là 512 bytes hoặc bội số của nó như 4096 bytes).

2. Logic Ứng dụng & Chiến lược Đệm (Application Layer)
Mục tiêu: Implement logic "Gom dữ liệu" (Batch Processing).

2.1. Định nghĩa Buffer & Ngưỡng (Threshold):

Tạo một RAM Ring Buffer (hoặc Double Buffer) dành riêng cho việc lưu tạm dữ liệu cảm biến.

Định nghĩa hằng số ngưỡng kích hoạt ghi SD: #define OFFLINE_FLUSH_THRESHOLD (100 * 1024) // 100 KB

2.2. Chiến lược ghi (Write Strategy):

Trạng thái Online:

Dữ liệu vẫn được gom thành các chunk vừa phải (ví dụ 4KB - 8KB) trước khi gửi đi hoặc ghi log, tránh gửi từng byte lẻ tẻ.

Trạng thái Offline (Mất mạng):

Bước 1: Dữ liệu mới đến -> Chỉ lưu vào RAM Buffer. Tuyệt đối không gọi lệnh fwrite xuống SD Card ngay.

Bước 2: Kiểm tra dung lượng Buffer hiện tại.

Bước 3:

Nếu Buffer_Size < 0.1 MB: Tiếp tục tích lũy.

Nếu Buffer_Size >= 0.1 MB: Thực hiện Bulk Write (Ghi một lần toàn bộ 100KB) xuống SD Card, sau đó giải phóng RAM.

Lưu ý an toàn: Cần một cơ chế kiểm tra tràn RAM (RAM Overflow protection). Nếu RAM sắp đầy trước khi đạt 0.1MB (trường hợp hiếm), vẫn phải force ghi xuống SD để tránh mất dữ liệu.

3. Mô hình hóa quy trình (Mermaid Diagrams)f
Vẽ sơ đồ logic cho việc xử lý dữ liệu khi mất mạng và ghi xuống thẻ nhớ.

## Cập nhật lại bootloader hỗ trợ nạp qua usb jtag
- cập nhật lại bootloader để hỗ trợ thêm nạp firmware qua usb jtag (thay vì chỉ mỗi uart như trước kia, tuy nhiên vẫn giữ lại logic như cũ)

## Thêm giao thức server HTTP, CoAP
- thêm giao thức HTTP để gửi dữ liệu lên server (mô tả cụ thể sau)
- thêm giao thức CoAP để gửi dữ liệu lên server (mô tả cụ thể sau)

## Viết firmware cho STM32WB55RG
- Viết firmware cho Zigbee cordinator dùng module STM32WB55RG (mô tả cụ thể sau)
- Viết firmware cho Bluetooth dùng module STM32WB55RG (mô tả cụ thể sau)
- Viết firmware cho Thread dùng module STM32WB55RG (mô tả cụ thể sau)

## viết firmware cho RA-08H module support LoRaWAN
- Viết firmware cho LoRaWAN dùng module RA-08H (mô tả cụ thể sau)

## Nâng cấp bảo mật cho gateway
- (mô tả cụ thể sau)

## Clean code, tối ưu hóa business logic
- (mô tả cụ thể sau)
## Nhận diện, scan gateway/các gateway trên module
- (mô tả cụ thể sau)


## Module base setting (sử dụng cho LAN, chưa áp dụng cho WAN)
- sử dụng file json để định dạng các tập lệnh của module theo hướng chức năng (thay vì chuẩn hóa về định dạng tập lệnh chung (kg khả thi), chuẩn hóa theo dạng chức năng của nó ví duk START, RESET, CONNECT_DEVICE, v.v..)
- chuẩn hóa theo dạng chức năng do tôi nhận thấy đa số các tập lệnh của các module đều có chức năng tương tự nhau (khoảng 70 - 80%), ví dụ như chức năng khởi động module, chức năng reset module, chức năng kết nối thiết bị, v.v.. nên việc chuẩn hóa theo dạng chức năng sẽ giúp việc phát triển firmware nhanh hơn và dễ dàng hơn rất nhiều so với việc chuẩn hóa theo định dạng tập lệnh chung (vì mỗi module có một định dạng tập lệnh khác nhau, rất khó để chuẩn hóa chung) 
- File json bao gồm: module type, module name vs id, communication port type (uart, spi, i2c, usb, ...), communication parameters (baudrate, parity, stopbit, ...), các chức năng tương úng với tập lệnh tương ứng theo dạng:
- tên chức năng
- cú pháp tập lệnh tương ứng (đối với các chức năng như reset hay sleep mà kg cần tập lệnh thì để trống)
- các gpio cần phải điều khiển tương ứng để chạy chức năng đó (nếu có) (lưu ý bao gồm cả trạng thái gpio đó là high hay low)
- thời gian delay cần chờ sau khi toogle gpio đó rồi mới chạy command (nếu có) (có thể có nhiều gpio cần toogle, liệt kê theo thứ tự)
- expect phản hồi của command (nếu có)
- thời gian timeout chờ phản hồi (nếu có)
- khi nhận phản hồi hoặc timeout thì gpio đó sẽ trở về trạng thái nào (high hay low, list ra nếu có)
- use case của file json: file json này giúp configuarable and modular gateway của tôi dễ dàng hơn, kg cần nạp / viết lại firmware cho từng module khác nhau, chỉ cần thay đổi file json tương ứng với module đó là được, giúp tiết kiệm thời gian và giảm độ sử dụng flash memory trên baseboard
cấu trúc file json đơn giản (chỉ cần nhiêu đây là đủ):
- module_id: "001" | "002" | ...
- module_type: "zigbee" | "bluetooth" | "lora" | ...
- module_name:
- module_communication:
  - port_type: "uart" | "spi" | "i2c" | "usb" | ...
  - parameters (example for uart):
      - baudrate: 115200
      - parity: "none"
      - stopbit: 1
- functions:
    - function_name: "START"
        command: "AT+START"
        gpio_start_control:
        pin: "001" state: "HIGH"
        pin: "002" state: "LOW"
        delay_start: 100
        expect_response: "OK"
        timeout: 500
        gpio_end_control:
        pin: "001" state: "LOW"
        pin: "002" state: "HIGH"
        delay_end: 100
    - function_name: "RESET"
        command: "" // no command needed
        gpio_start_control:
        pin: "003" state: "HIGH"
        delay_after: 200
        expect_response: ""
        timeout: 0
        gpio_end_control:
        pin: "003" state: "LOW"
        delay_end: 0
    - function_name: "SETTING"
        command: "AT+SET=PARAMS"
        gpio_start_control: []  
        delay_start: 0
        expect_response: "SET_OK"
        timeout: 1000
        gpio_end_control: []
        delay_end: 0

## FOR BLE:
Core function set (15 function)
1. Lifecycle (3)
MODULE_HW_RESET
Reset cứng GPIO (quan trọng nhất để recover lỗi).

MODULE_SW_RESET
Reset mềm qua lệnh (fallback nếu không có GPIO RST).

MODULE_FACTORY_RESET
Xóa config khi cần thiết lập lại từ đầu.

2. Info/Config (4)
MODULE_GET_INFO
Đọc version/model để verify module đúng.

MODULE_SET_NAME
Đặt tên thiết bị (thường cần cho deployment).

MODULE_SET_COMM_CONFIG
Cài baud/parity/stop (UART), clock (SPI), addr (I2C) – gộp tất cả comm setting vào 1 function.

MODULE_SET_RF_PARAMS
Cài TX power/channel – gộp tất cả RF setting vào 1 function.

3. Mode (2)
MODULE_ENTER_CMD_MODE
Chuyển sang nhận lệnh AT/command.

MODULE_ENTER_DATA_MODE
Chuyển sang transparent UART↔RF.

4. Connection (4)
MODULE_START_BROADCAST
Bật quảng bá (slave) hoặc permit join (coordinator).

MODULE_CONNECT
Kết nối đến địa chỉ (master/central).

MODULE_DISCONNECT
Ngắt kết nối.

MODULE_GET_CONNECTION_STATUS
Kiểm tra trạng thái link (idle/connected/error).

5. Power (2)
MODULE_ENTER_SLEEP
Vào sleep mode.

MODULE_WAKEUP
Đánh thức từ sleep.

Optional (thêm 5 nếu cần advanced)
MODULE_START_DISCOVERY (nếu làm central/coordinator scan network)

MODULE_SEND_DATA (nếu module không dùng transparent mà cần command-based send)

MODULE_GET_DIAGNOSTICS (đọc RSSI/link quality cho monitoring)

MODULE_SET_SECURITY_CONFIG (nếu cần pairing/PIN)

MODULE_ENTER_BOOTLOADER (nếu gateway phải OTA update module)

Các component mới trong các layer:
- BSP: USB/UART/SPI/I2C Module Communication: cung cấp các hàm giao tiếp cơ bản với module qua các giao thức khác nhau.
- BSP: IO Expander Driver: dùng để điểu khiển các GPIO của IO Expander, có thể copy từ stack handler cũ.
- Middleware: Json config Parser: phân tích file json để lấy cấu hình module và chức năng sau khi nhận được từ uart.
- Middleware: Module Config Controller: cung cấp các hàm điều khiển module dựa trên cấu hình từ file json (gọi hàm giao tiếp từ BSP và điều khiển GPIO từ IO Expander thay vì để ble/zigbee/lora handler phải làm điều đó).

công việc cần làm:
- xóa đi các package cũ trong BSP như ble handler, zigbee handler, lora handler
- viết các component mới như đã mô tả ở trên
- cập nhật lại config handler để đọc file json và lưu vào cấu trúc dữ liệu tương ứng
- ...
Flow file json: app sẽ chuyển json về dạng chuỗi ascii gửi uart xuống wan mcu, wan mcu xuống lan mcu, lan mcu áp dụng config
Mô tả cacs luôngf dữ liệu - luồng 1 (luồng config module): config json từ app -> chuyển qua uart -> đến wab mcu -> chuyển qua spi -> đến lan mcu -> parse json -> áp dụng config
luồng 2 (luồng dữ liệu cảm biến): dữ liệu cảm biến từ lan mcu -> chuyển qua spi -> đến wan mcu -> chuyển qua uart -> đến server
luông 3 (luồng dữ liệu điều khiển module): lệnh điều khiển module từ server -> đến wan mcu -> chuyển qua spi -> đến lan mcu -> áp dụng lệnh điều khiển module (dựa trên json config đã áp dụng trước đó)
luồng 4 (luồng dữ liệu từ app để scan và discovery device trên module): lệnh scan/discovery từ app -> đến wan mcu -> chuyển qua spi -> đến lan mcu -> thực hiện scan/discovery dựa trên json config đã áp dụng trước đó -> trả kết quả về app qua uart
luông 5 (luôngf các command setup như reset, set name, set rf params, etc..): lệnh setup từ app -> đến wan mcu -> chuyển qua spi -> đến lan mcu -> thực hiện lệnh setup dựa trên json config đã áp dụng trước đó -> trả kết quả về app qua uart
luồng 6: luồng dữ liệu từ server để scan và discovery device trên module: lệnh scan/discovery từ server -> đến wan mcu -> chuyển qua spi -> đến lan mcu -> thực hiện scan/discovery dựa trên json config đã áp dụng trước đó -> trả kết quả về server qua mqtt/http/coap
luông 7 (luồng các command setup như reset, set name, set rf params, etc..): lệnh setup từ server -> đến wan mcu -> chuyển qua spi -> đến lan mcu -> thực hiện lệnh setup dựa trên json config đã áp dụng trước đó -> trả kết quả về app qua mqtt/http/coap
Phân chia các nhóm chức năng dưới đây giúp tôi, nhóm có thể expect respone, kg thể expect response:
nhóm có thể expect:
JSON MAU BLE:
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
            "command": "",
            "gpio_start_control": [
                {
                    "pin": "RST",
                    "state": "LOW"
                }
            ],
            "delay_start": 100,
            "expect_response": "",
            "timeout": 0,
            "gpio_end_control": [
                {
                    "pin": "RST",
                    "state": "HIGH"
                }
            ],
            "delay_end": 1000
        },
        {
            "function_name": "MODULE_SW_RESET",
            "command": "AT+RESET\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 2000,
            "gpio_end_control": [],
            "delay_end": 1000
        },
        {
            "function_name": "MODULE_FACTORY_RESET",
            "command": "AT+RESTORE\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 3000,
            "gpio_end_control": [],
            "delay_end": 2000
        },
        {
            "function_name": "MODULE_GET_INFO",
            "command": "AT+VER\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+VER:",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_SET_NAME",
            "command": "AT+NAME=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_SET_COMM_CONFIG",
            "command": "AT+UART=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 100
        },
        {
            "function_name": "MODULE_SET_RF_PARAMS",
            "command": "AT+RF=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_ENTER_CMD_MODE",
            "command": "AT+CMDMODE\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_ENTER_DATA_MODE",
            "command": "AT+DATAMODE=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+DATAMODE",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_START_BROADCAST",
            "command": "AT+ADV=1\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 100
        },
        {
            "function_name": "MODULE_CONNECT",
            "command": "AT+CONNECT=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+CONNECTED:",
            "timeout": 5000,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_DISCONNECT",
            "command": "AT+DISCONNECT=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+DISCONNECTED:",
            "timeout": 1000,
            "gpio_end_control": [],
            "delay_end": 200
        },
        {
            "function_name": "MODULE_GET_CONNECTION_STATUS",
            "command": "AT+LIST\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+LIST:",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_ENTER_SLEEP",
            "command": "AT+SLEEP\r\n",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 100
        },
        {
            "function_name": "MODULE_WAKEUP",
            "command": "",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "",
            "timeout": 0,
            "gpio_end_control": [],
            "delay_end": 300
        },
        {
            "function_name": "MODULE_START_DISCOVERY",
            "command": "AT+SCAN=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+SCAN:",
            "timeout": 10000,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_SEND_DATA",
            "command": "AT+WRITE=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 1000,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_GET_DIAGNOSTICS",
            "command": "AT+INFO=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "+INFO:",
            "timeout": 500,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_DISCOVER_SERVICES",
            "command": "AT+DISC=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 5000,
            "gpio_end_control": [],
            "delay_end": 0
        },
        {
            "function_name": "MODULE_DISCOVER_CHARACTERISTICS",
            "command": "AT+CHARS=",
            "gpio_start_control": [],
            "delay_start": 0,
            "expect_response": "OK",
            "timeout": 5000,
            "gpio_end_control": [],
            "delay_end": 0
        }
    ]
}

### Next todo:
cụ thể:
- cập nhật lại cho LAN stack handler thêm gpio wake và reset còn thiếu (thêm vào thay vì chỉ 9 gpio như trước), thêm STACK_GPIO_PIN_WAKE, STACK_GPIO_PIN_PERST, đồng thời thêm vào parse, nhãn của chúng trong json sẽ là 0W (stack 1 chân WAKE), 0P (stack 1 chân PERST), tương tự cho stack 2
- giúp tôi thêm một chức năng như thế này mỗi lần khởi động module monitor task sẽ lấy id của các stack, thêm cho tôi một chức năng so sánh stack id cũ trong nvs, nếu chúng khác nhau thực hiện cập nhật stack id mới vào nvs và xóa json config cũ của stack nào có id khac trước (ví dụ khi get id của stack 1 khác id cũ của nó, xóa config json của stack 1, tuy nhiên nếu id của stack 2 vẫn giống với nvs thì giữ kg xóa config json)
- cập nhật lại stack handler cho WAN mcu, gpio mapping của stack handler như hình đã gửi (có cả wake và reset), WAN chỉ có một stack duy nhất, thêm cả điều khiển wake và reset
- với WAN do chỉ có một stack nên label của PIN sẽ là 01, 02, ..., 10, 11, WK, PE
- xóa bỏ hardcode các config của lte esp modem usb, thay vào đó tất cả đều để rỗng hết cho tôi, thêm vào lte task nếu các field này rỗng thì lte task kg được khởi động.
- xóa bỏ hard code field name của module thay vào đó sẽ được nhét vào ở command, field name hardcode:
```c
/* ==================== Modem Target Selection ==================== */
#define CONFIG_MODEM_TARGET_A7600C1      1

#if defined(CONFIG_MODEM_TARGET_USER)
    #define CONFIG_MODEM_TARGET_NAME               "User Defined"
#elif defined(CONFIG_MODEM_TARGET_NT26)
    #define  CONFIG_MODEM_TARGET_NAME               "NT26"
#elif defined(CONFIG_MODEM_TARGET_ML302_DNLM)
    #define  CONFIG_MODEM_TARGET_NAME               "ML302-DNLM/CNLM"
#elif defined(CONFIG_MODEM_TARGET_AIR724UG_NFM)
    #define CONFIG_MODEM_TARGET_NAME               "AIR724UG-NFM"
#elif defined(CONFIG_MODEM_TARGET_AIR780_E)
    #define CONFIG_MODEM_TARGET_NAME               "AIR780E"
#elif defined(CONFIG_MODEM_TARGET_EC600N_CNLA_N05)
    #define CONFIG_MODEM_TARGET_NAME               "EC600NCNLA-N05"
#elif defined(CONFIG_MODEM_TARGET_EC600N_CNLC_N06)
    #define CONFIG_MODEM_TARGET_NAME               "EC600NCNLC-N06"
#elif defined(CONFIG_MODEM_TARGET_A7600C1)
    #define CONFIG_MODEM_TARGET_NAME               "A7600C1"
#elif defined(CONFIG_MODEM_TARGET_BG95_M3)
    #define CONFIG_MODEM_TARGET_NAME               "BG95M3"
#elif defined(CONFIG_MODEM_TARGET_BG96_MA)
    #define CONFIG_MODEM_TARGET_NAME               "BG96MA"
#elif defined(CONFIG_MODEM_TARGET_MC610_EU)
    #define CONFIG_MODEM_TARGET_NAME               "MC610_EU"
#elif defined(CONFIG_MODEM_TARGET_EC20_CE)
    #define CONFIG_MODEM_TARGET_NAME               "EC20_CE"
#elif defined(CONFIG_MODEM_TARGET_EG25_GL)
    #define CONFIG_MODEM_TARGET_NAME               "EG25_GL"
#elif defined(CONFIG_MODEM_TARGET_YM310_X09)
    #define CONFIG_MODEM_TARGET_NAME               "YM310_X09"
#elif defined(CONFIG_MODEM_TARGET_SIM7600E)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7600E"
#elif defined(CONFIG_MODEM_TARGET_A7670E)
    #define CONFIG_MODEM_TARGET_NAME               "A7670E"
#elif defined(CONFIG_MODEM_TARGET_SIM7070G)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7070G"
#elif defined(CONFIG_MODEM_TARGET_SIM7080)
    #define CONFIG_MODEM_TARGET_NAME               "SIM7080G"
#else  // Default
    #define CONFIG_MODEM_TARGET_NAME               "ML302-DNLM/CNLM"
#endif
```
- cập nhật thêm hàm gọi stack get id cho WAN, gọi hàm này ở main luôn (cũng làm persuado như LAN, trả về lưu vào g_stack_id_wan = 001), lưu ý đây là var sẽ lưu vào nvs cũng làm tương tự logic trên.
- cập nhật lại thêm command cho lte modem để set các chân này dựa trên gpio mapping trên:
```c
#ifndef CONFIG_MODEM_POWER_GPIO
    #define CONFIG_MODEM_POWER_GPIO         22
#endif
#define MODEM_GPIO_POWER                    CONFIG_MODEM_POWER_GPIO

#ifndef CONFIG_MODEM_RESET_GPIO
    #define CONFIG_MODEM_RESET_GPIO         23
#endif
#define MODEM_GPIO_RESET                    CONFIG_MODEM_RESET_GPIO

#ifndef CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL
    #define CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL  1
#endif
#define MODEM_GPIO_POWER_INACTIVE_LEVEL     CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL

#ifndef CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL
    #define CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL  1
#endif
#define MODEM_GPIO_RESET_INACTIVE_LEVEL     CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL
```
thêm vào chuỗi này:
```c
/**
 * @brief Parse LTE configuration from command string
 * Format: "LT:MODEM_NAME:TYPE:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:RECONNECT_TIMEOUT:MAX_RECONNECT:PWR_PIN:RST_PIN"
 * Example: "LT:A7600C1:v-internet:user:pass:USB:true:30000:0:WK:PE"
 * Note: Username and password can be empty
 * @param data Raw command data
 * @param len Command length
 * @param cfg Output LTE config structure
 * @return ESP_OK on success, ESP_FAIL on error
 */
```
- sau khi có các phần gpio trên, thêm vào esp_modem_usb, cập nhật lại như sau, thay vì dùng gpio của esp32 thì dùng của tca.
(chưa test)
-  cập nhật lại app config, check lại nếu stack id nào hiện thì mối hiện config cho stack đó (miêu tả sau)