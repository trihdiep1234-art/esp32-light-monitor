# Nghiên cứu và Thiết lập Hệ thống Đo lường Suy hao Quang học

## Giới thiệu tổng quan
Kho lưu trữ này chứa toàn bộ mã nguồn của dự án đo lường sự suy giảm cường độ ánh sáng qua các lớp môi trường ô nhiễm giả lập. Hệ thống được thiết kế dựa trên vi điều khiển ESP32 và ứng dụng nền tảng hệ điều hành thời gian thực FreeRTOS nhằm đảm bảo độ chính xác cao trong chu kỳ thu thập dữ liệu. Dữ liệu thực nghiệm đo đạc được sẽ làm cơ sở toán học để đánh giá hệ số xuyên thấu, phục vụ trực tiếp cho công tác cảnh báo và bảo trì các hệ thống năng lượng quang điện.

## Kiến trúc phần cứng
Hệ thống đo lường được cấu thành từ các linh kiện tiêu chuẩn:
* **Khối xử lý trung tâm:** Vi điều khiển ESP32 tích hợp khả năng kết nối mạng không dây.
* **Khối đo lường kỹ thuật số:** Cảm biến cường độ rọi BH1750 sử dụng chuẩn giao tiếp đồng bộ I2C.
* **Khối đo lường tương tự:** Cảm biến ánh sáng TEMT6000 kết nối qua bộ chuyển đổi số hóa ADC.
* **Khối hiển thị cục bộ:** Vi mạch điều khiển màn hình OLED SSD1306 phục vụ công tác giám sát tại chỗ.

## Kiến trúc phần mềm
Mã nguồn dự án được phát triển bằng ngôn ngữ C trên lõi ESP-IDF. Thuật toán cốt lõi sử dụng bộ định thời phần cứng kết hợp cơ chế cờ sự kiện EventGroup để đồng bộ hóa các tác vụ đọc cảm biến ở tần số chuẩn 1 Hz. Khoảng xê dịch thời gian giữa các chu kỳ lấy mẫu được hệ thống tự động tính toán liên tục nhằm giám sát độ trễ pha của hệ điều hành.

Toàn bộ thông số môi trường thô sau khi thu thập sẽ được đóng gói theo định dạng cấu trúc JSON. Dữ liệu này tiếp tục được đẩy lên máy chủ đám mây ThingsBoard thông qua giao thức truyền thông nhẹ MQTT để tiến hành trực quan hóa và phân tích tương quan.

## Hướng dẫn cài đặt và vận hành
Để biên dịch dự án, máy tính phát triển cần được thiết lập sẵn môi trường ESP-IDF.

1. Tải toàn bộ mã nguồn từ kho lưu trữ về máy tính cục bộ.
2. Cập nhật thông tin mạng mạng không dây và địa chỉ máy chủ MQTT bên trong tệp tin mã nguồn cốt lõi.
3. Thực thi lệnh biên dịch toàn bộ dự án: `idf.py build`
4. Tiến hành nạp chương trình xuống vi điều khiển: `idf.py -p PORT flash`
5. Khởi chạy giao diện giám sát để kiểm tra luồng dữ liệu thời gian thực: `idf.py -p PORT monitor`

## Đơn vị triển khai
Dự án thực nghiệm này được phát triển bởi nhóm sinh viên thuộc Đại học Bách khoa Hà Nội, dưới sự cố vấn chuyên môn và định hướng kỹ thuật của TS. Nguyễn Nam Phong.