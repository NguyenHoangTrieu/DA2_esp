# RGB LED Status Mapping

File điều khiển chính: `main/DA2_esp.c`

RGB LED đang dùng 3 chân IOX sau:

- `P12 = RLED`
- `P13 = GLED`
- `P14 = BLED`

Lưu ý phần cứng:

- LED là active-low.
- Trong code, `false = LED channel ON`, `true = LED channel OFF`.

## Quy tắc ưu tiên

Nếu nhiều trạng thái xuất hiện cùng lúc thì LED sẽ hiển thị theo thứ tự ưu tiên từ trên xuống dưới.

1. Pin critical thấp
2. Lỗi internet/server
3. Pin thấp
4. Config mode
5. Hoạt động bình thường

## Bảng trạng thái

| Trạng thái | Màu / kiểu nháy | Ý nghĩa |
| --- | --- | --- |
| Pin critical thấp | Đỏ nháy nhanh | Pin rất thấp, cần sạc hoặc cấp nguồn ngoài sớm |
| WAN có nhưng chưa lên server | Xanh dương nháy chậm | WiFi/LTE/Ethernet đã có link, nhưng MQTT/HTTP/CoAP chưa xác nhận online với server |
| Mất WAN hoàn toàn | Tím nháy nhanh | Chưa có kết nối internet vật lý/logical từ WiFi, LTE hoặc Ethernet |
| Pin thấp | Vàng nháy chậm | Pin còn thấp, nên sạc sớm |
| CONFIG mode | Cyan nháy chậm | Thiết bị đang ở chế độ cấu hình web/AP |
| Bình thường | Xanh lá sáng liên tục | Hệ thống đang hoạt động bình thường và đã online tới server |

## Điều kiện xác định

- `Pin critical thấp`:
  - pin có mặt, và
  - `Vbat <= PWR_BATT_LOWER_THRESHOLD_MV`, hoặc
  - `SoC <= PWR_BATT_CRITICAL_SOC_PCT`
- `Pin thấp`:
  - `SoC <= PWR_BATT_LOW_SOC_PCT`
- `WAN có nhưng chưa lên server`:
  - `is_internet_connected = true`
  - nhưng `mcu_lan_handler_get_internet_status() != INTERNET_STATUS_ONLINE`
- `Mất WAN hoàn toàn`:
  - `is_internet_connected = false`
- `Bình thường`:
  - server online và không có cảnh báo pin ưu tiên cao hơn

## Ghi chú thiết kế

- Trước đây RGB bị dùng chung như trạng thái `battery_source_enabled`, nên dễ xung đột với cảnh báo vận hành.
- Hiện tại RGB được tách riêng thành lớp hiển thị trạng thái định kỳ.
- Khi mất VBUS, quạt và rail `EN_5V` vẫn bị tắt để tiết kiệm năng lượng, nhưng RGB vẫn được giữ lại để hiển thị cảnh báo ngắn.
- Cờ `SOCF` thô của BQ27441 không còn được dùng trực tiếp cho LED, vì nó có thể còn giữ trạng thái cũ sau khi pin đã hồi phục hoặc sau khi SoC được sửa bằng OCV fallback.