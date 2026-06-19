#ifndef SSD1306_H
#define SSD1306_H

#include "i2cdev.h" // Nhập thẳng thư viện i2cdev quản lý chung
#include <stdbool.h>
#include <stdint.h>

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64

typedef struct
{
    i2c_dev_t dev; // Sử dụng struct thiết bị của i2cdev thay vì driver gốc
    uint8_t width;
    uint8_t height;
} SSD1306_t;

void ssd1306_init(SSD1306_t *dev, uint8_t width, uint8_t height, int sda, int scl);
void ssd1306_clear_screen(SSD1306_t *dev, bool invert);
void ssd1306_display_text(SSD1306_t *dev, int page, const char *text, int len, bool invert);

#endif // SSD1306_H