# main/CMakeLists.txt

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        bh1750 
        temt6000 
        ssd1306 
        i2cdev
        driver
        esp_adc
)