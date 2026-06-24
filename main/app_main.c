#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" 
#include "esp_log.h"

#include "i2cdev.h"
#include "bh1750.h"
#include "temt6000.h"
#include "ssd1306.h"

#include "mqtt_client.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

// ================= WIFI + THINGSBOARD =================
// do ESP32 được sử dụng là thế hệ cũ nên chỉ sử dụng băng tần 2.4KHz 
// --> sử dụng mạng băng tần 2.4 KHz để kết nối và gửi dữ liệu len ThingsBoard
#define WIFI_SSID      "Die"
#define WIFI_PASS      "@Diepne79"

#define TB_TOKEN       "e71btpt0iljlhwbuwihn"    // Access Token của Device trên ThingsBoard
#define TB_SERVER      "thingsboard.cloud"       // MQTT Broker của ThingsBoard

#define I2C_MASTER_NUM I2C_NUM_0
#define SDA_GPIO 21
#define SCL_GPIO 22

static const char *TAG = "MAIN";
static esp_mqtt_client_handle_t mqtt_client;

// Chương trình chỉ tiếp tục khi ESP32 đã nhận được địa chỉ IP.
#define WIFI_CONNECTED_BIT BIT0
// Bit báo đã đến chu kỳ lấy mẫu
#define TIMER_TRIGGER_BIT BIT1
static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_timer_event_group;

// Chuyển khai báo biến toàn cục lên đây để các hàm WiFi bên dưới có thể dùng được biến oled
i2c_dev_t bh1750_dev; 
TEMT6000_t temt_sensor;
SSD1306_t oled;

// WiFi Event Handler xử lý các sự kiện phát sinh từ WiFi Stack:
// - STA_START        : bắt đầu kết nối WiFi
// - STA_DISCONNECTED : tự động kết nối lại khi mất mạng
// - GOT_IP           : đã nhận được địa chỉ IP từ Router
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    // chờ khi nào nhận được IP mạng mới gửi dữ liệu
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();

        ESP_LOGW(TAG, "DANG KET NOI LAI WIFI...");

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
        ssd1306_display_text(&oled, 2, "WIFI ERROR", 10, false);
        ssd1306_display_text(&oled, 4, "Reconnect...", 12, false);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
        ssd1306_display_text(&oled, 2, "WIFI OK", 7, false);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_timer_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta(); 

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Đợi tối đa 10 giây cho đến khi ESP32 nhận được IP.
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000)   // chờ tối đa 10 giây
    );

    if(bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "WiFi Connected");

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
        ssd1306_display_text(&oled, 2, "WIFI OK", 7, false);
    }
    else
    {
        ESP_LOGE(TAG, "WiFi Timeout");

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
        ssd1306_display_text(&oled, 2, "WIFI ERROR", 10, false);
    }
}

// Khi MQTT_EVENT_CONNECTED xuất hiện nghĩa là ESP32 đã kết nối thành công tới ThingsBoard.
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    switch(event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT CONNECTED");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT DISCONNECTED");
            break;
        default:
            break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://thingsboard.cloud",  
// MQTT Broker URI.
// ESP32 sẽ gửi dữ liệu tới ThingsBoard thông qua giao thức MQTT.
// ThingsBoard xác thực thiết bị bằng Access Token.
// Token đóng vai trò như username.
        .credentials.username = TB_TOKEN,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ====================================================
// Callback được gọi mỗi 1 giây
// KHÔNG đọc cảm biến trong callback
// Chỉ phát tín hiệu cho Task chính
// ====================================================
static void sensor_timer_callback(void *arg)
{
    xEventGroupSetBits(
        s_timer_event_group,
        TIMER_TRIGGER_BIT
    );
}

void app_main(void)
{
    ESP_LOGI(TAG, "System Start");

    // ================= I2C INIT =================
    ESP_ERROR_CHECK(i2cdev_init());

    // ================= OLED INIT =================
    ssd1306_init(&oled, 128, 64, SDA_GPIO, SCL_GPIO);
    ssd1306_clear_screen(&oled, false);

    // Hiển thị trạng thái khởi động
    ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
    ssd1306_display_text(&oled, 2, "Connecting WiFi", 15, false);

    // ================= WIFI + MQTT INIT =================
    wifi_init();
    mqtt_init();

    // 1. Khởi tạo thông số mô tả mặc định cho BH1750
    // Địa chỉ mặc định của BH1750 là 0x23 (ADDR nối GND).
    // Nếu ADDR nối VCC thì địa chỉ sẽ là 0x5C.
    ESP_ERROR_CHECK(bh1750_init_desc(
        &bh1750_dev,
        BH1750_ADDR_LO,
        I2C_MASTER_NUM,
        SDA_GPIO,
        SCL_GPIO));

    // 2. THAY ĐỔI QUAN TRỌNG: Ghi đè tốc độ I2C xuống 50kHz trước khi setup chip
    // Giảm tốc độ I2C từ 400kHz xuống 50kHz để tăng độ ổn định.
    bh1750_dev.cfg.master.clk_speed = 50000; 

    // Tiến hành thiết lập cấu hình hoạt động cho BH1750
    if (bh1750_setup(&bh1750_dev, BH1750_MODE_CONTINUOUS, BH1750_RES_HIGH) != ESP_OK)
    {
        ESP_LOGE(TAG, "KHONG THE THIET LAP BH1750! Vui long kiem tra lai day va dien tro pull-up.");
    }
    
    // TEMT6000 là cảm biến ánh sáng analog.
    // ESP32 đọc giá trị thông qua bộ ADC tích hợp.
    if (temt6000__Init(&temt_sensor, ADC_UNIT_1, ADC_CHANNEL_6) != TEMT6000_OK)
    {
        ESP_LOGE(TAG, "TEMT6000 init failed");
    }

    ESP_LOGI(TAG, "All sensors ready");
    
// ====================================================
// Tạo ESP Timer định kỳ 1 giây
// ====================================================

    esp_timer_handle_t sensor_timer;

    const esp_timer_create_args_t timer_args = {
        .callback = &sensor_timer_callback,
        .name = "sensor_timer"
    };

    ESP_ERROR_CHECK(
        esp_timer_create(
            &timer_args,
            &sensor_timer
        )
    );

// 1 000 000 us = 1 giây
    ESP_ERROR_CHECK(
        esp_timer_start_periodic(
            sensor_timer,
            1000000
        )
    );

    while (1)
    {
        // ==========================================
        // Chờ Timer báo tới chu kỳ đo
        // ==========================================
        xEventGroupWaitBits(
            s_timer_event_group,
            TIMER_TRIGGER_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );
        
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG,
                 "Sample Time = %.3f s",
                 now / 1000000.0);
        
        uint16_t bh_lux = 0;
        TEMT6000_illuminance_t temt_lux = 0;

        // ================= READ SENSOR =================
        bh1750_read(&bh1750_dev, &bh_lux);
        temt6000__ReadIlluminance(&temt_sensor, 10, &temt_lux);

        // ================= TÍNH TOÁN =================
        float lux = (float)bh_lux;
        ESP_LOGI(TAG, "BH1750=%.2f | TEMT=%.2f", lux, (float)temt_lux);

        // ================= OLED DISPLAY =================
        char line1[32], line2[32]; 

        snprintf(line1, sizeof(line1), "BH:%d lx", bh_lux);
        snprintf(line2, sizeof(line2), "TEMT:%.1f", (float)temt_lux);

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 14, false);
        ssd1306_display_text(&oled, 2, line1, strlen(line1), false);
        ssd1306_display_text(&oled, 3, line2, strlen(line2), false);
        

        // ================= THINGSBOARD JSON =================
        // Chuyển dữ liệu cảm biến sang định dạng JSON.
        // Đây là định dạng ThingsBoard sử dụng để lưu Telemetry.
        char payload[250];
        snprintf(payload, sizeof(payload), "{\"lux\":%d,\"temt\":%.2f}", bh_lux, (float)temt_lux);

        ESP_LOGI(TAG, "SEND: %s", payload);

        // ================= SEND MQTT =================
        // Mọi dữ liệu gửi vào topic này sẽ được ThingsBoard lưu
        // dưới dạng Telemetry của thiết bị.
        esp_mqtt_client_publish(
            mqtt_client,
            "v1/devices/me/telemetry",
            payload,
            0,
            1,
            0
        );
    }
}