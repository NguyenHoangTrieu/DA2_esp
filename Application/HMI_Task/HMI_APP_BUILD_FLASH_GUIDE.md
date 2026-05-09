# HMI App Build and Flash Guide

Guide nay mo ta contract HMI hien tai cua DA2: man TJC chi giu 1 page trang `home`, con toan bo dashboard duoc firmware ve bang code.

## 1. Contract HMI hien tai

Firmware hien tai yeu cau:

- Display model: `TJC3224K024_011`
- Orientation: `Landscape (90 degrees)`
- UART baud rate: `115200`
- Chi co 1 page: `home`
- `home` phai la page index `0`
- Nen `home` la trang (`bco = 65535`)
- Khong co button
- Khong co text component tinh
- Khong co page `pgWifi`, `pgLTE`, `pgKB`
- Font index `0` phai ton tai de firmware dung lenh `xstr`

Neu sai bat ky diem nao o tren, firmware van gui lenh duoc nhung giao dien co the trang tron, sai font, hoac khong dung contract mong muon.

## 2. Nguon file can quan ly

Thu muc lien quan:

- `DA2_esp/Application/HMI_Task/`
- `DA2_esp/Middleware/HMI_Display/`
- `DA2_esp/BSP/hmi_handler/`
- `HMI_Project/`

Khuyen nghi:

- Chi giu 1 file `.HMI` chinh thuc cho blank-page design
- Dat ten ro rang cho file `.tft` xuat ra, vi du `DA2_gateway_blank_YYYYMMDD.tft`

## 3. Cach tao dung HMI app trong TJC Editor

1. Tao project moi cho `TJC3224K024_011` o che do `Landscape` va baud `115200`.
2. Import `font_ascii_16.zi` vao font index `0`.
3. Xoa tat ca page phu, chi de lai `home`.
4. Dat `home.bco = 65535`.
5. Khong them bat ky component nao len `home`.
6. Save project va compile ra `.tft`.

Day la y do: TJC chi la canvas UART, khong con giu logic UI bang component nua.

## 4. Flash `.tft` len man hinh

### Cach khuyen dung: microSD

1. Format microSD thanh `FAT32`.
2. Copy duy nhat 1 file `.tft` vao root card.
3. Tat nguon man hinh.
4. Cam the vao module TJC.
5. Cap nguon `5V` on dinh.
6. Doi update xong va man reboot.
7. Rut the nho sau khi flash xong.

Luu y:

- Khong de nhieu file `.tft` trong root card.
- Khong cap nguon bang 3.3V GPIO yeu.

## 5. Firmware se ve nhung gi

Sau khi DA2 firmware vao HMI mode, page `home` se duoc firmware ve cac muc sau:

- `DATN_GATEWAY`
- ngay va gio
- pin: `%`, `mV`, `Charging/Idle`, thanh battery
- internet: loai ket noi va `ONLINE/OFFLINE`
- server: loai server va `CONNECTED/DISCONNECTED`
- link web config: `http://gateway.local/` hoac `http://192.168.4.1/`

Layout nay hien do file `DA2_esp/Middleware/HMI_Display/src/hmi_display.c` quyet dinh.

## 6. Kiem tra sau khi flash

Log mong doi o ESP32:

```text
I HMI_TASK: HMI mode active
I HMI_DISP: goto_page home
I HMI_DISP: refresh bat=93% inet=WIFI/1 server=MQTT/1 time=27/05/2026 14:52:09
```

Kiem tra tren man hinh:

1. Nen trang hien ngay khi boot vao HMI mode.
2. Dashboard xuat hien ma khong can component TJC nao.
3. Gia tri pin thay doi theo `pwr_monitor_task`.
4. URL web config hien dung theo mode AP/STA.

## 7. Loi thuong gap

### Man hinh chi hien nen trang

Thuong do mot trong cac nguyen nhan sau:

- page khong ten `home`
- baud rate khong phai `115200`
- chua import font index `0`
- firmware chua vao `hmi_task_enter_mode()`

### Build duoc `.tft` nhung giao dien lai giong ban cu

Ban da compile nham file `.HMI` co button/page cu. Can thong nhat 1 file project blank-page duy nhat trong `HMI_Project/`.

### Muon doi bo cuc

Khong sua bang TJC component. Sua truc tiep firmware renderer trong `hmi_display.c` va flash lai firmware.