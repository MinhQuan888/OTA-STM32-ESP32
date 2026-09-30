# OTA Firmware Update System for STM32 via ESP32

**Đề tài NCKH Sinh viên** — Mã số: **SV2026-230**  
**Tên đề tài:** Triển khai thử nghiệm kỹ thuật OTA trên kit nhúng  
**Sinh viên:** Hoàng Mạnh Hùng (23139017) — Lớp 23139A, Khoa Điện – Điện tử, HCMUTE  
**Người hướng dẫn:** ThS. Huỳnh Thị Thu Hiền

---

## Mô tả hệ thống

Hệ thống cập nhật firmware không dây (OTA) cho vi điều khiển **STM32F103C8T6** thông qua **ESP32** làm Wi-Fi Gateway. Người dùng tải file `.bin` lên giao diện Web, ESP32 lưu vào SPIFFS và truyền xuống STM32 qua UART với cơ chế xác nhận ACK/NACK.

```
[Trình duyệt Web] → Wi-Fi → [ESP32 Gateway] → UART → [STM32 Bootloader] → Flash
```

---

## Cấu trúc thư mục

```
BOOTLOADER_TEST/
├── Application_OTA/     # Firmware ứng dụng mẫu chạy trên STM32 (LED nhấp nháy)
├── Bootloader_OTA/      # Chương trình Bootloader STM32F103C8T6
│   └── Core/Src/main.c  # Hàm enter_to_app(), Is_App_Valid(), vòng lặp OTA
├── ESP32_OTA/           # Chương trình Gateway ESP32 (Web Server + SPIFFS + UART)
└── hex_to_bin/          # Tiện ích chuyển đổi .hex → .bin
```

---

## Thông số kỹ thuật

| Thông số | Giá trị |
|---|---|
| Vi điều khiển đích | STM32F103C8T6 (128KB Flash, 20KB RAM) |
| Gateway | ESP32-WROOM-32 |
| Giao thức truyền | UART 115200 baud |
| Cấu trúc khung | `[SEQ 1B][DATA 256B][CRC 1B]` |
| Kiểm tra lỗi | CRC per-frame (XOR) + Firmware checksum (fw_sum) |
| Cơ chế phản hồi | Stop-and-Wait, ACK(0x06)/NACK(0x15), tối đa 5 lần retry |
| Phân vùng Flash | Bootloader: `0x08000000` (~50KB), Application: `0x0800C800` (~78KB) |
| Giao diện người dùng | Web Server HTTP trên ESP32 (cùng mạng LAN) |
| Lưu trữ trung gian | SPIFFS trên Flash ESP32 |

---

## Kết quả thực nghiệm

| Test Case | Mô tả | Kết quả |
|---|---|---|
| TC1 | Cập nhật firmware bình thường | ✅ PASS — tỉ lệ thành công 100% (50/50 lần) |
| TC2 | Cố tình gây lỗi CRC ở khối thứ 3 | ✅ PASS — STM32 gửi NACK, abort sau 5 retry |
| TC3 | Không gửi firmware (timeout 3 giây) | ✅ PASS — Bootloader nhảy vào App cũ |

---

## Sơ đồ kết nối phần cứng

```
ESP32          STM32F103C8T6
GPIO17 (TX) →  PA10 (UART1 RX)
GPIO16 (RX) ←  PA9  (UART1 TX)
GND         ─  GND
```

---

## Hướng dẫn sử dụng

1. Nạp `Bootloader_OTA` vào STM32 bằng ST-Link
2. Nạp `ESP32_OTA` vào ESP32 qua cổng USB
3. Cấp nguồn cả hai board, kết nối cùng mạng Wi-Fi
4. Mở trình duyệt → nhập IP của ESP32 (hiển thị trên Serial Monitor)
5. Chọn file `.bin` và nhấn **Upload**
6. Hệ thống tự động cập nhật và khởi động lại STM32
