idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
        mqtt
        driver
        esp_adc
        bh1750
        temt6000
        i2cdev
        ssd1306
)