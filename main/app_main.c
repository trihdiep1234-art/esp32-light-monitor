#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "i2cdev.h"
#include "bh1750.h"
#include "temt6000.h"
#include "ssd1306.h"

#define I2C_MASTER_NUM I2C_NUM_0
#define SDA_GPIO 21
#define SCL_GPIO 22

static const char *TAG = "MAIN";

i2c_dev_t bh1750_dev;
TEMT6000_t temt_sensor;
SSD1306_t oled;

void app_main(void)
{
    ESP_LOGI(TAG, "System Start");

    ESP_ERROR_CHECK(i2cdev_init());

    ESP_ERROR_CHECK(bh1750_init_desc(
        &bh1750_dev,
        BH1750_ADDR_LO,
        I2C_MASTER_NUM,
        SDA_GPIO,
        SCL_GPIO));

    ESP_ERROR_CHECK(bh1750_setup(
        &bh1750_dev,
        BH1750_MODE_CONTINUOUS,
        BH1750_RES_HIGH));

    if (temt6000__Init(&temt_sensor, ADC_UNIT_1, ADC_CHANNEL_6) != TEMT6000_OK)
    {
        ESP_LOGE(TAG, "TEMT6000 init failed");
    }

    ssd1306_init(&oled, 128, 64, SDA_GPIO, SCL_GPIO);

    ssd1306_clear_screen(&oled, false);

    ESP_LOGI(TAG, "All sensors ready");

    while (1)
    {
        uint16_t bh_lux = 0;
        TEMT6000_illuminance_t temt_lux = 0;

        if (bh1750_read(&bh1750_dev, &bh_lux) != ESP_OK)
        {
            ESP_LOGW(TAG, "BH1750 read fail");
        }

        if (temt6000__ReadIlluminance(&temt_sensor, 10, &temt_lux) != TEMT6000_OK)
        {
            ESP_LOGW(TAG, "TEMT6000 read fail");
        }

        ESP_LOGI(TAG, "BH1750 = %d Lux | TEMT6000 = %.2f Lux", bh_lux, temt_lux);

        char line1[32];
        char line2[32];

        snprintf(line1, sizeof(line1), "BH1750:%d lx", bh_lux);
        snprintf(line2, sizeof(line2), "TEMT:%.1f lx", temt_lux);

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT SENSOR", 12, false);
        ssd1306_display_text(&oled, 2, line1, strlen(line1), false);
        ssd1306_display_text(&oled, 4, line2, strlen(line2), false);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}