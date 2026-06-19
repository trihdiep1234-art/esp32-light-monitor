#include "ssd1306.h"
#include "esp_log.h"
#include <string.h>

// THÊM DÒNG NÀY ĐỂ KẾT NỐI BỘ FONT CHỮ
#include "font8x8_basic.h" 

static const char *TAG = "SSD1306";
#define SSD1306_ADDR 0x3C

static void i2c_write(SSD1306_t *dev, uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t buf[129];
    buf[0] = control;
    if (len > 0) {
        memcpy(&buf[1], data, len);
    }
    
    // Gửi dữ liệu qua i2cdev 
    i2c_dev_write(&dev->dev, NULL, 0, buf, len + 1);
}

void ssd1306_init(SSD1306_t *dev, uint8_t width, uint8_t height, int sda, int scl)
{
    // ... (Giữ nguyên nội dung hàm init như cũ) ...
    dev->width = width;
    dev->height = height;

    dev->dev.port = I2C_NUM_0;
    dev->dev.addr = SSD1306_ADDR;
    dev->dev.cfg.sda_io_num = sda;
    dev->dev.cfg.scl_io_num = scl;
    dev->dev.cfg.master.clk_speed = 400000;

    i2c_dev_create_mutex(&dev->dev);

    uint8_t init_cmds[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };

    i2c_write(dev, 0x00, init_cmds, sizeof(init_cmds));
    ESP_LOGI(TAG, "SSD1306 initialized flawlessly via i2cdev!");
}

void ssd1306_clear_screen(SSD1306_t *dev, bool invert)
{
    // ... (Giữ nguyên nội dung hàm clear_screen như cũ) ...
    uint8_t zero[SSD1306_WIDTH];
    memset(zero, invert ? 0xFF : 0x00, SSD1306_WIDTH);

    for (int page = 0; page < 8; page++)
    {
        uint8_t cmd[] = { 0xB0 + page, 0x00, 0x10 };
        i2c_write(dev, 0x00, cmd, sizeof(cmd));
        i2c_write(dev, 0x40, zero, SSD1306_WIDTH);
    }
}

// THAY THẾ TOÀN BỘ HÀM HIỂN THỊ CHỮ BẰNG HÀM MỚI NÀY
void ssd1306_display_text(SSD1306_t *dev, int page, const char *text, int len, bool invert)
{
    if (page >= 8) return; // Màn hình chỉ có 8 page (0-7), chống ghi đè lỗi

    uint8_t buffer[SSD1306_WIDTH];
    memset(buffer, invert ? 0xFF : 0x00, SSD1306_WIDTH);

    // Duyệt qua từng ký tự trong chuỗi text truyền vào
    for (int i = 0; i < len; i++)
    {
        uint8_t char_code = (uint8_t)text[i];

        // Chỉ xử lý các ký tự có trong bảng mã ASCII cơ bản (0 - 127)
        if (char_code < 128)
        {
            // Mỗi ký tự font 8x8 chiếm 8 cột pixel
            for (int col = 0; col < 8; col++)
            {
                int screen_pos = (i * 8) + col; 
                
                // Tránh vẽ tràn ra ngoài chiều rộng màn hình (128 pixel)
                if (screen_pos < SSD1306_WIDTH) 
                {
                    uint8_t pixel_data = font8x8_basic_tr[char_code][col];
                    buffer[screen_pos] = invert ? ~pixel_data : pixel_data;
                }
            }
        }
    }

    // Gửi lệnh set địa chỉ dòng (Page)
    uint8_t cmd[] = { 0xB0 + page, 0x00, 0x10 };
    i2c_write(dev, 0x00, cmd, sizeof(cmd));
    
    // Đẩy toàn bộ dòng data (128 pixel) ra màn hình
    i2c_write(dev, 0x40, buffer, SSD1306_WIDTH);
}