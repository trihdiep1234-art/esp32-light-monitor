#include <stdio.h>
#include <string.h>
#include <math.h> 

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
// Do ESP32 được sử dụng là thế hệ cũ nên chỉ sử dụng băng tần 2.4GHz 
// --> sử dụng mạng băng tần 2.4 KHz để kết nối và gửi dữ liệu lên ThingsBoard
#define WIFI_SSID      "Die"
#define WIFI_PASS      "@Diepne79"

#define TB_TOKEN       "e71btpt0iljlhwbuwihn"    // Access Token của Device trên ThingsBoard
#define TB_SERVER      "thingsboard.cloud"       // MQTT Broker của ThingsBoard

#define I2C_MASTER_NUM I2C_NUM_0
#define SDA_GPIO 21
#define SCL_GPIO 22

// ⏱️ Chu kỳ thời gian thí nghiệm
#define CALIB_WINDOW_SEC   60  // 1 phút đầu lưu giá trị nền kính sạch cho CẢ HAI con
#define MEASURE_WINDOW_SEC 120  // 2 phút sau chốt số liệu so sánh một lần và gửi ThingsBoard

static const char *TAG = "MAIN";
static esp_mqtt_client_handle_t mqtt_client;

// Chương trình chỉ tiếp tục khi ESP32 đã nhận được địa chỉ IP.
#define WIFI_CONNECTED_BIT BIT0
// Bit báo đã đến chu kỳ lấy mẫu (1 giây)
#define TIMER_TRIGGER_BIT BIT1
static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_timer_event_group;

// Biến toàn cục cho thiết bị ngoại vi
i2c_dev_t bh1750_dev; 
TEMT6000_t temt_sensor;
SSD1306_t oled;

typedef enum { SYS_CALIBRATING = 0, SYS_MEASURING } sys_mode_t;
typedef enum { DUST_CLEAN = 0, DUST_LIGHT, DUST_MEDIUM, DUST_HEAVY } dust_level_t;

const char* dust_str[] = {"CLEAN", "LIGHT", "MEDIUM", "HEAVY"};

// WiFi Event Handler xử lý các sự kiện phát sinh từ WiFi Stack
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
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
    
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch(event_id) {
        case MQTT_EVENT_CONNECTED: ESP_LOGI(TAG, "MQTT CONNECTED"); break;
        case MQTT_EVENT_DISCONNECTED: ESP_LOGW(TAG, "MQTT DISCONNECTED"); break;
        default: break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://thingsboard.cloud",  
        .credentials.username = TB_TOKEN,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Callback được gọi mỗi 1 giây từ Hardware Timer phần cứng
static void sensor_timer_callback(void *arg)
{
    xEventGroupSetBits(s_timer_event_group, TIMER_TRIGGER_BIT);
}

void app_main(void)
{
    ESP_LOGI(TAG, "System Start");

    ESP_ERROR_CHECK(i2cdev_init());
    ssd1306_init(&oled, 128, 64, SDA_GPIO, SCL_GPIO);
    ssd1306_clear_screen(&oled, false);
    ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
    ssd1306_display_text(&oled, 2, "Connecting WiFi", 15, false);

    wifi_init();
    mqtt_init();

    ESP_ERROR_CHECK(bh1750_init_desc(&bh1750_dev, BH1750_ADDR_LO, I2C_MASTER_NUM, SDA_GPIO, SCL_GPIO));
    bh1750_dev.cfg.master.clk_speed = 50000; // Giảm xuống 50kHz tăng độ ổn định đường truyền

    if (bh1750_setup(&bh1750_dev, BH1750_MODE_CONTINUOUS, BH1750_RES_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "KHONG THE THIET LAP BH1750!");
    }
    
    if (temt6000__Init(&temt_sensor, ADC_UNIT_1, ADC_CHANNEL_6) != TEMT6000_OK) {
        ESP_LOGE(TAG, "TEMT6000 init failed");
    }

    ESP_LOGI(TAG, "All sensors ready");
    
    esp_timer_handle_t sensor_timer;
    const esp_timer_create_args_t timer_args = { .callback = &sensor_timer_callback, .name = "sensor_timer" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sensor_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_timer, 1000000)); // 1 giây kích ngắt một lần

    sys_mode_t current_mode = SYS_CALIBRATING;
    int time_counter = 0;
    float temt_f = -1.0f;

    // Các biến lưu Baseline độc lập của từng cảm biến
    double calib_accum_bh = 0.0;
    double calib_accum_temt = 0.0;
    float base_bh = 0.0f;
    float base_temt = 0.0f;

    // Các biến tích lũy chu kỳ đo 5 phút tiếp theo
    double measure_accum_bh = 0.0;
    double measure_accum_temt = 0.0;

    // Biến lưu kết quả chốt chu kỳ
    float final_loss_bh = 0.0f;
    float final_loss_temt = 0.0f;
    dust_level_t status_bh = DUST_CLEAN;
    dust_level_t status_temt = DUST_CLEAN;

    while (1)
    {
        // Hệ thống chờ ngắt từ Timer phần cứng (1 giây thức dậy một lần)
        xEventGroupWaitBits(s_timer_event_group, TIMER_TRIGGER_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        
        uint16_t bh_lux = 0;
        TEMT6000_illuminance_t temt_lux = 0;

        // 1. Đọc dữ liệu từ cảm biến
        bh1750_read(&bh1750_dev, &bh_lux);
        temt6000__ReadIlluminance(&temt_sensor, 10, &temt_lux);

        // Bộ lọc số EMA cho TEMT6000
        if (temt_f < 0) temt_f = (float)temt_lux;
        else temt_f = (temt_f * 0.8f) + ((float)temt_lux * 0.2f);

        time_counter++;

        // ================= LUỒNG 1: GỬI DỮ LIỆU REALTIME (MỖI 1 GIÂY) =================
        // Luồng này chạy liên tục từ giây đầu tiên để Thingsboard vẽ đồ thị không bị ngắt quãng
        char rt_payload[128];
        snprintf(rt_payload, sizeof(rt_payload), "{\"lux\":%d,\"temt\":%.2f}", bh_lux, temt_f);
        esp_mqtt_client_publish(mqtt_client, "v1/devices/me/telemetry", rt_payload, 0, 1, 0);


        // Các mảng chuỗi phục vụ hiển thị OLED cục bộ
        char line_0[32], line_1[32], line_2[32], line_3[32];

        // ================= GIAI ĐOẠN 1: TỰ LẤY BASELINE ĐỘC LẬP (2 PHÚT ĐẦU) =================
        if (current_mode == SYS_CALIBRATING) 
        {
            calib_accum_bh += bh_lux;
            calib_accum_temt += temt_f;

            snprintf(line_0, sizeof(line_0), "CALIBRATING...");
            snprintf(line_1, sizeof(line_1), "B_RT:%d lx", bh_lux);
            snprintf(line_2, sizeof(line_2), "T_RT:%.0f lx", temt_f);
            snprintf(line_3, sizeof(line_3), "Time: %ds/%ds", time_counter, CALIB_WINDOW_SEC);

            if (time_counter >= CALIB_WINDOW_SEC) 
            {
                base_bh = (float)(calib_accum_bh / time_counter);
                base_temt = (float)(calib_accum_temt / time_counter);
                
                ESP_LOGW(TAG, "=== HIỆU CHUẨN XONG ===");
                current_mode = SYS_MEASURING;
                time_counter = 0; // Reset bộ đếm chuyển sang chu kỳ đo 3 phút
            }
        }
        // ================= GIAI ĐOẠN 2: ĐO ĐẠC SUY HAO ĐỘC LẬP (MỖI 3 PHÚT) =================
        else 
        {
            measure_accum_bh += bh_lux;
            measure_accum_temt += temt_f;

            // Tính toán % Loss realtime phục vụ hiển thị OLED nhanh
            float inst_loss_bh = (bh_lux < base_bh && base_bh > 0) ? ((base_bh - bh_lux) / base_bh * 100.0f) : 0.0f;
            float inst_loss_temt = (temt_f < base_temt && base_temt > 0) ? ((base_temt - temt_f) / base_temt * 100.0f) : 0.0f;

            snprintf(line_0, sizeof(line_0), "L_BH RT: %.1f%%", inst_loss_bh);
            snprintf(line_1, sizeof(line_1), "L_TM RT: %.1f%%", inst_loss_temt);
            snprintf(line_2, sizeof(line_2), "Update: %ds/%ds", time_counter, MEASURE_WINDOW_SEC);
            snprintf(line_3, sizeof(line_3), "B:%s T:%s", dust_str[status_bh], dust_str[status_temt]);

            // ================= LUỒNG 2: CHỐT CHU KỲ BÁO CÁO (MỖI 3 PHÚT) =================
            if (time_counter >= MEASURE_WINDOW_SEC) 
            {
                float avg_bh = (float)(measure_accum_bh / time_counter);
                float avg_temt = (float)(measure_accum_temt / time_counter);
                
                final_loss_bh = (avg_bh < base_bh && base_bh > 0) ? ((base_bh - avg_bh) / base_bh * 100.0f) : 0.0f;
                final_loss_temt = (avg_temt < base_temt && base_temt > 0) ? ((base_temt - avg_temt) / base_temt * 100.0f) : 0.0f;

                if (final_loss_bh < 1.0f)       status_bh = DUST_CLEAN;
                else if (final_loss_bh < 3.0f) status_bh = DUST_LIGHT;
                else if (final_loss_bh < 7.0f) status_bh = DUST_MEDIUM;
                else                            status_bh = DUST_HEAVY;

                if (final_loss_temt < 1.0f)       status_temt = DUST_CLEAN;
                else if (final_loss_temt < 3.0f) status_temt = DUST_LIGHT;
                else if (final_loss_temt < 7.0f) status_temt = DUST_MEDIUM;
                else                              status_temt = DUST_HEAVY;

                // Gửi gói tổng hợp chứa kết quả tính toán chu kỳ lên Thingsboard
                char report_payload[350]; 
                snprintf(report_payload, sizeof(report_payload), 
                         "{\"base_bh\":%.1f,\"base_temt\":%.1f,\"avg_bh\":%.1f,\"avg_temt\":%.1f,"
                         "\"loss_bh\":%.2f,\"loss_temt\":%.2f,\"status_bh\":\"%s\",\"status_temt\":\"%s\"}", 
                         base_bh, base_temt, avg_bh, avg_temt, 
                         final_loss_bh, final_loss_temt, 
                         dust_str[status_bh], dust_str[status_temt]);

                ESP_LOGW(TAG, "=== GỬI BÁO CÁO CHỐT 3 PHÚT ===");
                esp_mqtt_client_publish(mqtt_client, "v1/devices/me/telemetry", report_payload, 0, 1, 0);

                // Giải phóng bộ tích lũy chu kỳ
                measure_accum_bh = 0.0;
                measure_accum_temt = 0.0;
                time_counter = 0; 
            }
        }

        // Cập nhật màn hình OLED cục bộ
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, line_0, strlen(line_0), false);
        ssd1306_display_text(&oled, 2, line_1, strlen(line_1), false);
        ssd1306_display_text(&oled, 4, line_2, strlen(line_2), false);
        ssd1306_display_text(&oled, 6, line_3, strlen(line_3), false);
    }
}