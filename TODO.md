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

3. Mô hình hóa quy trình (Mermaid Diagrams)
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

## Clean code, tối ưu hóa logic nghiệp vụ
- (mô tả cụ thể sau)
## Nhận diện, scan gateway/các gateway trên module
- (mô tả cụ thể sau)