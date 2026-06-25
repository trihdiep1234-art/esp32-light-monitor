# ESP32 Light Monitor

Project giám sát ánh sáng môi trường dùng ESP32, so sánh 2 cảm biến (BH1750 - digital,
TEMT6000 - analog), hiển thị trên OLED và gửi dữ liệu lên ThingsBoard qua MQTT.

## 1. Phần cứng

| Thiết bị        | Giao tiếp | Chân ESP32                |
|-----------------|-----------|----------------------------|
| BH1750          | I2C       | SDA = GPIO21, SCL = GPIO22 (địa chỉ 0x23, ADDR nối GND) |
| OLED SSD1306    | I2C (chung bus với BH1750) | SDA = GPIO21, SCL = GPIO22 (địa chỉ 0x3C) |
| TEMT6000        | ADC (analog) | GPIO34 (ADC1 Channel 6) |

BH1750 và OLED dùng **chung 1 bus I2C** (`I2C_NUM_0`) nhưng tốc độ clock khác nhau
(BH1750 đọc ở 50kHz, OLED ở 400kHz) — bus tự chuyển tốc độ giữa 2 thiết bị, xem mục 6.

## 2. Cài ESP-IDF (chỉ cần làm 1 lần)

Project build với **ESP-IDF v5.3.5**, target chip **esp32** (Xtensa).

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
```

Mỗi khi mở terminal mới, phải nạp lại biến môi trường trước khi dùng `idf.py`:
```bash
. $HOME/esp/esp-idf/export.sh
```
Mẹo: thêm `alias get_idf='. $HOME/esp/esp-idf/export.sh'` vào `~/.bashrc` để chỉ cần gõ `get_idf`.

## 3. Clone project và cấu hình

```bash
git clone https://github.com/trihdiep1234-art/esp32-light-monitor.git
cd esp32-light-monitor
```

Mở `main/app_main.c`, sửa lại các thông số đầu file cho đúng môi trường của bạn:
```c
#define WIFI_SSID      "..."   // tên WiFi (chỉ hỗ trợ băng tần 2.4GHz)
#define WIFI_PASS      "..."   // mật khẩu WiFi
#define TB_TOKEN       "..."   // Access Token của Device trên ThingsBoard
```

> ⚠️ **Lưu ý bảo mật**: đừng commit WiFi password / token thật lên repo public.
> Nếu repo đã từng có thông tin thật, hãy đổi mật khẩu WiFi và rotate lại token
> ThingsBoard trước khi nộp bài.

## 4. Build, cấp quyền cổng serial, flash

**Cấp quyền đọc cổng USB-serial (chỉ cần làm 1 lần / máy):**
```bash
sudo usermod -aG dialout $USER
```
Sau lệnh này phải **logout/login lại** (hoặc `sudo reboot`) để group có hiệu lực.
Kiểm tra: `groups $USER` phải thấy `dialout` trong danh sách.

**Build:**
```bash
idf.py set-target esp32
idf.py build
```

**Xác định cổng serial của board:**
```bash
ls -l /dev/ttyUSB* /dev/ttyACM*   # Linux
```

**Flash + xem log:**
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```
(thay `/dev/ttyUSB0` bằng cổng thật; thoát monitor bằng `Ctrl+]`)

## 5. Đo sai số tần số lấy mẫu (log `[TIMING]`)

Mỗi giây, firmware tự log dòng:
```
I (xxxx) MAIN: [TIMING] delta_us=999987 | dt=0.999987s | f_actual=1.000013Hz | error=-0.00130%
```
- `delta_us` / `dt`: khoảng cách thực tế giữa 2 lần lấy mẫu liên tiếp (so với 1.000s danh định).
- `f_actual`: tần số lấy mẫu thực tế.
- `error`: % lệch so với tần số danh định 1Hz.

Để lấy số liệu cho báo cáo: chạy `idf.py monitor`, để chạy ít nhất 60-100 giây,
copy các dòng `[TIMING]` ra, tính trung bình/độ lệch chuẩn của `dt` trong Excel.

Lọc riêng dòng này khi xem log:
```bash
idf.py -p /dev/ttyUSB0 monitor | grep TIMING
```

## 6. Các vấn đề/giới hạn đã biết

- **TEMT6000 bị bão hoà (saturate) ở ~618.4 lux**: điện áp ra của cảm biến
  (qua điện trở tải 10kΏ) vượt quá dải đo của ADC (~3.1V ở `ADC_ATTEN_DB_11`)
  khi ánh sáng đủ mạnh — ADC bị "kịch kim" ở giá trị raw tối đa 4095, nên lux
  tính ra luôn dừng ở mức trần ~618.4 dù ánh sáng thực tế có mạnh hơn.
  Firmware hiện tự log cảnh báo khi việc này xảy ra:
  ```
  W (xxxx) TEMT6000: ADC BAO HOA (raw=4095/4095, mV=3092) -> lux=618.4 la GIOI HAN PHAN CUNG, KHONG phai gia tri sang thuc te!
  ```
  Đây là giới hạn của mạch cảm biến (điện trở tải 10kΏ), không sửa được
  thuần bằng phần mềm — muốn đo được mức sáng cao hơn cần giảm giá trị
  điện trở tải hoặc thêm cầu phân áp trước ADC.

- **WiFi có thể reconnect định kỳ** (`DANG KET NOI LAI WIFI...`) nếu access
  point là hotspot điện thoại có chế độ tiết kiệm pin — không ảnh hưởng đến
  độ chính xác lấy mẫu (timer chạy độc lập với WiFi) nhưng có thể làm rớt
  vài gói MQTT. Nên test với router cố định trước khi demo.

## 7. Cấu trúc project

```
main/app_main.c        - entry point: WiFi, MQTT, đọc cảm biến, OLED, gửi ThingsBoard
components/bh1750/     - driver BH1750 (digital lux sensor, I2C)
components/temt6000/   - driver TEMT6000 (analog ambient light sensor, ADC)
components/ssd1306/    - driver OLED 128x64
components/i2cdev/     - lớp trừu tượng I2C dùng chung cho BH1750 + OLED
```