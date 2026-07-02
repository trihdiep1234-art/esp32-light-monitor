#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_sleep.h"

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

#include "sds011.h"
#include "sds011_structs.h"
#include "sds011_consts.h"

// ================= WIFI + THINGSBOARD =================
#define WIFI_SSID  "Die"
#define WIFI_PASS  "@Diepne79"
#define TB_TOKEN   "uVhdpDogaRagnIG0aR08"
#define TB_SERVER  "thingsboard.cloud"

// ================= CHÂN GPIO =================
#define I2C_MASTER_NUM          I2C_NUM_0
#define SDA_GPIO                21
#define SCL_GPIO                22
#define BH1750_I2C_CLK_SPEED_HZ 50000
#define SDS011_TX_GPIO          GPIO_NUM_25
#define SDS011_RX_GPIO          GPIO_NUM_26

// ================= OLED LAYOUT =================
// SSD1306 128x64 có 8 page (0-7), mỗi page cao 8px.
// Bố cục cố định — chỉ vẽ lại đúng page có số liệu thay đổi,
// không clear toàn màn hình mỗi chu kỳ để giảm giao dịch I2C.
//
//  Page 0: "AIR MONITOR"     header, cố định
//  Page 2: BH1750 lux
//  Page 3: TEMT6000 lux
//  Page 5: PM2.5 (µg/m³)
//  Page 6: PM10  (µg/m³)
#define OLED_PAGE_HEADER 0
#define OLED_PAGE_BH     2
#define OLED_PAGE_TEMT   3
#define OLED_PAGE_PM25   5
#define OLED_PAGE_PM10   6

// ====================================================
// CHẾ ĐỘ NGỦ / ĐO LUÂN PHIÊN — LÝ DO CHỌN THAM SỐ
// ----------------------------------------------------
// Tổng chu kỳ: 5 phút = 300 giây
//
// ACTIVE (60s):
//   - SDS011 cần ~30s warmup sau khi thoát sleep để ổn định laser và quạt
//     trước khi cho dữ liệu PM chính xác (datasheet Nova SDS011 trang 4).
//   - 30s đầu: chỉ lấy ánh sáng (BH1750 + TEMT6000), đánh dấu PM là "warmup".
//   - 30s sau: đọc đầy đủ cả PM2.5/PM10.
//   - 60s đủ để có ít nhất 30 mẫu ánh sáng + 30 mẫu PM hợp lệ mỗi chu kỳ.
    //
// SLEEP (240s):
//   - Kéo dài 4 phút để tiết kiệm điện và kéo dài tuổi thọ laser SDS011
//     (datasheet khuyến nghị không đo liên tục > vài giờ).
//   - Ánh sáng môi trường biến đổi chậm (phòng, nhà kính...) nên 5 phút
//     một lần đo là đủ để theo dõi xu hướng.
//   - ESP32 dùng light sleep trong thời gian này: giảm dòng tiêu thụ
//     từ ~160mA xuống ~0.8mA, WiFi/MQTT vẫn duy trì được.
// ====================================================
#define ACTIVE_DURATION_S  180
#define SLEEP_DURATION_S   15
#define SDS011_WARMUP_S    15

// ================= EVENT GROUP BITS =================
#define WIFI_CONNECTED_BIT  BIT0
#define TIMER_TRIGGER_BIT   BIT1   // 1Hz sensor tick (chỉ active trong ACTIVE phase)
#define PHASE_CHANGE_BIT    BIT2   // chuyển trạng thái ACTIVE <-> SLEEP

// ================= TRẠNG THÁI HỆ THỐNG =================
typedef enum {
    STATE_ACTIVE,    // đang đo
    STATE_SLEEPING   // đang ngủ
} system_state_t;

static const char *TAG = "MAIN";
static esp_mqtt_client_handle_t mqtt_client;

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_timer_event_group;

static esp_timer_handle_t sensor_timer;  // 1Hz, chỉ chạy khi ACTIVE
static esp_timer_handle_t phase_timer;   // one-shot, kích hoạt chuyển phase

i2c_dev_t  bh1750_dev;
TEMT6000_t temt_sensor;
SSD1306_t  oled;

// ====================================================
// Hàm gửi lệnh sleep/wake cho SDS011
// Tách ra để gọi lại khi chuyển trạng thái
// ====================================================
static void sds011_set_sleep(bool sleep_on)
{
    struct sds011_tx_packet cmd = {0};
    cmd.head        = SDS011_PACKET_HEAD;
    cmd.command     = SDS011_CMD_TX;
    cmd.sub_command = SDS011_TX_CMD_SLEEP_MODE;
    cmd.payload_sleep_mode.method = SDS011_METHOD_SET;
    cmd.payload_sleep_mode.mode   = sleep_on
                                    ? SDS011_SLEEP_MODE_ENABLED
                                    : SDS011_SLEEP_MODE_DISABLED;
    cmd.device_id = SDS011_ID_ALL;
    cmd.tail      = SDS011_PACKET_TAIL;
    sds011_send_cmd_to_queue(&cmd, pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "SDS011: %s", sleep_on ? "SLEEP" : "WAKE");
}

// ====================================================
// Timer callbacks — KHÔNG làm việc nặng ở đây,
// chỉ set event group bit để task chính xử lý
// ====================================================
static void sensor_timer_callback(void *arg)
{
    xEventGroupSetBits(s_timer_event_group, TIMER_TRIGGER_BIT);
}

static void phase_timer_callback(void *arg)
{
    xEventGroupSetBits(s_timer_event_group, PHASE_CHANGE_BIT);
}

// ====================================================
// WiFi Event Handler
// ====================================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGW(TAG, "DANG KET NOI LAI WIFI...");
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);
        ssd1306_display_text(&oled, 2, "WIFI ERROR", 10, false);
        ssd1306_display_text(&oled, 4, "Reconnect...", 12, false);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);
        ssd1306_display_text(&oled, 2, "WIFI OK", 7, false);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group  = xEventGroupCreate();
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
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi Connected");
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);
        ssd1306_display_text(&oled, 2, "WIFI OK", 7, false);
    } else {
        ESP_LOGE(TAG, "WiFi Timeout");
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);
        ssd1306_display_text(&oled, 2, "WIFI ERROR", 10, false);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:    ESP_LOGI(TAG, "MQTT CONNECTED");    break;
        case MQTT_EVENT_DISCONNECTED: ESP_LOGW(TAG, "MQTT DISCONNECTED"); break;
        default: break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri  = "mqtt://thingsboard.cloud",
        .credentials.username = TB_TOKEN,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ====================================================
// Chuyển sang trạng thái ACTIVE
// ====================================================
static void enter_active(void)
{
    ESP_LOGI(TAG, "[PHASE] -> ACTIVE (%ds)", ACTIVE_DURATION_S);

    // Wake SDS011 trước — cần ~30s warmup
    sds011_set_sleep(false);

    // Hiển thị trạng thái trên OLED
    ssd1306_display_text(&oled, 7, "MODE:ACTIVE ", 12, false);

    // Bật sensor_timer 1Hz
    esp_timer_start_periodic(sensor_timer, 1000000);

    // Đặt phase_timer one-shot để chuyển sang SLEEP sau ACTIVE_DURATION_S
    esp_timer_start_once(phase_timer,
                         (uint64_t)ACTIVE_DURATION_S * 1000000ULL);
}

// ====================================================
// Chuyển sang trạng thái SLEEP
// ====================================================
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "[PHASE] -> SLEEP (%ds)", SLEEP_DURATION_S);

    // Dừng sensor_timer để không đọc cảm biến trong lúc ngủ
    esp_timer_stop(sensor_timer);

    // Gửi SDS011 vào sleep mode
    sds011_set_sleep(true);

    // Hiển thị trạng thái trên OLED
    ssd1306_display_text(&oled, 7, "MODE:SLEEP  ", 12, false);

    // Đặt phase_timer one-shot để thức dậy sau SLEEP_DURATION_S
    esp_timer_start_once(phase_timer,
                         (uint64_t)SLEEP_DURATION_S * 1000000ULL);

    // ESP32 light sleep: giảm dòng tiêu thụ ~160mA -> ~0.8mA
    // Không phải vTaskDelay — CPU thực sự dừng, chỉ timer hardware chạy.
    // WiFi/MQTT tự reconnect sau khi thức dậy qua wifi_event_handler.
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_S * 1000000ULL);
    esp_light_sleep_start();
    // Chương trình tiếp tục từ đây sau khi thức dậy
    ESP_LOGI(TAG, "[PHASE] ESP32 da thuc day tu light sleep");
}

// ====================================================
// app_main
// ====================================================
void app_main(void)
{
    ESP_LOGI(TAG, "System Start");

    // ================= I2C =================
    ESP_ERROR_CHECK(i2cdev_init());

    // ================= OLED =================
    ssd1306_init(&oled, 128, 64, SDA_GPIO, SCL_GPIO);
    ssd1306_clear_screen(&oled, false);
    ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);
    ssd1306_display_text(&oled, 2, "Connecting WiFi", 15, false);

    // ================= WIFI + MQTT =================
    wifi_init();
    mqtt_init();

    // ================= BH1750 =================
    ESP_ERROR_CHECK(bh1750_init_desc(
        &bh1750_dev, BH1750_ADDR_LO, I2C_MASTER_NUM, SDA_GPIO, SCL_GPIO));
    bh1750_dev.cfg.master.clk_speed = BH1750_I2C_CLK_SPEED_HZ;
    if (bh1750_setup(&bh1750_dev, BH1750_MODE_CONTINUOUS, BH1750_RES_HIGH) != ESP_OK)
        ESP_LOGE(TAG, "KHONG THE THIET LAP BH1750!");

    // ================= TEMT6000 =================
    if (temt6000__Init(&temt_sensor, ADC_UNIT_1, ADC_CHANNEL_6) != TEMT6000_OK)
        ESP_LOGE(TAG, "TEMT6000 init failed");

    // ================= SDS011 =================
    // vTaskDelay bên dưới là của driver SDS011 (bên thứ 3), bắt buộc để
    // UART task nội bộ khởi động xong trước khi nhận lệnh — không phải
    // delay trong vòng lặp lấy mẫu của nhóm.
    sds011_begin(UART_NUM_1, SDS011_TX_GPIO, SDS011_RX_GPIO);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Set active report mode
    struct sds011_tx_packet cmd = {0};
    cmd.head        = SDS011_PACKET_HEAD;
    cmd.command     = SDS011_CMD_TX;
    cmd.sub_command = SDS011_TX_CMD_REPORT_MODE;
    cmd.payload_report_mode.method = SDS011_METHOD_SET;
    cmd.payload_report_mode.mode   = SDS011_REPORT_MODE_ACTIVE;
    cmd.device_id   = SDS011_ID_ALL;
    cmd.tail        = SDS011_PACKET_TAIL;
    sds011_send_cmd_to_queue(&cmd, pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "SDS011: ACTIVE report mode");

    // ================= OLED: vẽ header + nhãn 1 lần =================
    ssd1306_clear_screen(&oled, false);
    ssd1306_display_text(&oled, OLED_PAGE_HEADER, "AIR MONITOR", 11, false);

    // ================= TẠO TIMERS =================
    // sensor_timer: 1Hz periodic — chỉ bật trong STATE_ACTIVE
    const esp_timer_create_args_t sensor_args = {
        .callback = &sensor_timer_callback,
        .name     = "sensor_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&sensor_args, &sensor_timer));

    // phase_timer: one-shot — kiểm soát chuyển ACTIVE <-> SLEEP
    const esp_timer_create_args_t phase_args = {
        .callback = &phase_timer_callback,
        .name     = "phase_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&phase_args, &phase_timer));

    // ================= BIẾN ĐO SÁI SỐ =================
    static int64_t last_sample_time = 0;
    double dt_sec    = 1.0;
    double f_actual  = 1.0;
    double error_pct = 0.0;
    int    active_tick = 0;   // đếm số chu kỳ 1Hz trong ACTIVE phase

    float pm25 = 0.0f;
    float pm10 = 0.0f;

    system_state_t state = STATE_SLEEPING; // khởi đầu là SLEEPING để enter_active() init đúng

    // Khởi động chu kỳ đầu tiên: vào ACTIVE ngay lập tức
    state = STATE_ACTIVE;
    active_tick = 0;
    last_sample_time = 0;
    enter_active();

    // ====================================================
    // VÒNG LẶP CHÍNH
    // ====================================================
    while (1)
    {
        // Chờ 1 trong 2 bit: sensor tick (1Hz) hoặc phase change
        EventBits_t bits = xEventGroupWaitBits(
            s_timer_event_group,
            TIMER_TRIGGER_BIT | PHASE_CHANGE_BIT,
            pdTRUE,    // clear bits sau khi nhận
            pdFALSE,   // chờ bất kỳ bit nào
            portMAX_DELAY);

        // ==========================================
        // XỬ LÝ CHUYỂN TRẠNG THÁI
        // ==========================================
        if (bits & PHASE_CHANGE_BIT) {
            if (state == STATE_ACTIVE) {
                // ACTIVE xong -> vào SLEEP
                state = STATE_SLEEPING;
                active_tick = 0;
                enter_sleep();
                // Sau khi thức dậy từ light sleep, chuyển ngay sang ACTIVE
                state = STATE_ACTIVE;
                active_tick = 0;
                last_sample_time = 0;
                enter_active();
            }
            // (Không cần nhánh STATE_SLEEPING ở đây vì enter_sleep() đã
            // block tại esp_light_sleep_start() rồi tự gọi enter_active())
            continue;
        }

        // ==========================================
        // XỬ LÝ SENSOR TICK (1Hz, chỉ khi ACTIVE)
        // ==========================================
        if ((bits & TIMER_TRIGGER_BIT) && (state == STATE_ACTIVE)) {
            active_tick++;

            // ===== ĐO SAI SỐ TẦN SỐ LẤY MẪU =====
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "Sample Time = %.3f s | Tick #%d/%d",
                     now / 1000000.0, active_tick, ACTIVE_DURATION_S);

            if (last_sample_time != 0) {
                dt_sec    = (now - last_sample_time) / 1000000.0;
                f_actual  = (dt_sec > 0.0) ? (1.0 / dt_sec) : 0.0;
                error_pct = ((dt_sec - 1.0) / 1.0) * 100.0;
                ESP_LOGI(TAG, "[TIMING] dt=%.4fs | f_actual=%.4fHz | error=%.4f%%",
                         dt_sec, f_actual, error_pct);
            }
            last_sample_time = now;

            // ===== ĐỌC ÁNH SÁNG (luôn đọc trong cả 60s) =====
            uint16_t bh_lux = 0;
            TEMT6000_illuminance_t temt_lux = 0;
            bh1750_read(&bh1750_dev, &bh_lux);
            temt6000__ReadIlluminance(&temt_sensor, 10, &temt_lux);

            // ===== ĐỌC BỤI (chỉ sau khi SDS011 đã warmup xong) =====
            bool pm_valid = (active_tick > SDS011_WARMUP_S);
            if (pm_valid) {
                struct sds011_rx_packet dust_packet;
                if (sds011_recv_data_from_queue(&dust_packet, pdMS_TO_TICKS(100)) == SDS011_OK) {
                    pm25 = ((dust_packet.payload_query_data.pm2_5_high << 8)
                          |  dust_packet.payload_query_data.pm2_5_low) / 10.0f;
                    pm10 = ((dust_packet.payload_query_data.pm10_high << 8)
                          |  dust_packet.payload_query_data.pm10_low) / 10.0f;
                    ESP_LOGI(TAG, "PM2.5=%.1f µg/m³ | PM10=%.1f µg/m³", pm25, pm10);
                }
            } else {
                ESP_LOGI(TAG, "SDS011 warmup... (%ds/%ds)", active_tick, SDS011_WARMUP_S);
            }

            ESP_LOGI(TAG, "BH=%.2f lx | TEMT=%.2f lx | PM2.5=%.1f | PM10=%.1f",
                     (float)bh_lux, (float)temt_lux, pm25, pm10);

            // ===== OLED =====
            char line_bh[16], line_temt[16], line_pm25[16], line_pm10[16];
            snprintf(line_bh,   sizeof(line_bh),   "BH:%d lx",   bh_lux);
            snprintf(line_temt, sizeof(line_temt),  "TEMT:%.1f",  (float)temt_lux);
            snprintf(line_pm25, sizeof(line_pm25),  pm_valid ? "PM25:%.1f" : "PM25:warm", pm25);
            snprintf(line_pm10, sizeof(line_pm10),  pm_valid ? "PM10:%.1f" : "PM10:warm", pm10);

            ssd1306_display_text(&oled, OLED_PAGE_BH,   line_bh,   strlen(line_bh),   false);
            ssd1306_display_text(&oled, OLED_PAGE_TEMT,  line_temt, strlen(line_temt), false);
            ssd1306_display_text(&oled, OLED_PAGE_PM25,  line_pm25, strlen(line_pm25), false);
            ssd1306_display_text(&oled, OLED_PAGE_PM10,  line_pm10, strlen(line_pm10), false);

            // ===== MQTT (chỉ gửi khi có ít nhất ánh sáng hợp lệ) =====
            char payload[300];
            snprintf(payload, sizeof(payload),
                     "{\"lux\":%d,\"temt\":%.2f,"
                     "\"pm25\":%.1f,\"pm10\":%.1f,\"pm_valid\":%s,"
                     "\"dt\":%.4f,\"f_actual\":%.4f,\"error_pct\":%.4f}",
                     bh_lux, (float)temt_lux,
                     pm25, pm10, pm_valid ? "true" : "false",
                     dt_sec, f_actual, error_pct);

            ESP_LOGI(TAG, "SEND: %s", payload);
            esp_mqtt_client_publish(
                mqtt_client, "v1/devices/me/telemetry",
                payload, 0, 1, 0);
        }
    }
}