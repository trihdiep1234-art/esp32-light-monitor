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

#include "sds011.h"
#include "sds011_structs.h"
#include "sds011_consts.h"

#define WIFI_SSID      "Die"
#define WIFI_PASS      "@Diepne79"
#define TB_TOKEN       "e71btpt0iljlhwbuwihn"
#define TB_SERVER      "thingsboard.cloud"

#define I2C_MASTER_NUM I2C_NUM_0
#define SDA_GPIO 21
#define SCL_GPIO 22

// GPIO đã xác nhận hoạt động qua loopback test
#define SDS011_TX_GPIO GPIO_NUM_25
#define SDS011_RX_GPIO GPIO_NUM_26

static const char *TAG = "MAIN";
static esp_mqtt_client_handle_t mqtt_client;

#define WIFI_CONNECTED_BIT BIT0
#define TIMER_TRIGGER_BIT  BIT1
static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_timer_event_group;

i2c_dev_t   bh1750_dev;
TEMT6000_t  temt_sensor;
SSD1306_t   oled;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
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
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
        ssd1306_display_text(&oled, 2, "WIFI OK", 7, false);
    } else {
        ESP_LOGE(TAG, "WiFi Timeout");
        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 13, false);
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

    ESP_ERROR_CHECK(bh1750_init_desc(
        &bh1750_dev, BH1750_ADDR_LO, I2C_MASTER_NUM, SDA_GPIO, SCL_GPIO));
    bh1750_dev.cfg.master.clk_speed = 50000;
    if (bh1750_setup(&bh1750_dev, BH1750_MODE_CONTINUOUS, BH1750_RES_HIGH) != ESP_OK)
        ESP_LOGE(TAG, "KHONG THE THIET LAP BH1750!");

    if (temt6000__Init(&temt_sensor, ADC_UNIT_1, ADC_CHANNEL_6) != TEMT6000_OK)
        ESP_LOGE(TAG, "TEMT6000 init failed");

    // SDS011: UART1, TX=GPIO25, RX=GPIO26
    sds011_begin(UART_NUM_1, SDS011_TX_GPIO, SDS011_RX_GPIO);
    ESP_LOGI(TAG, "SDS011 ready on UART1 TX=GPIO25 RX=GPIO26");

    // Đợi task RX/TX khởi động xong
    vTaskDelay(pdMS_TO_TICKS(500));

    // Lệnh 1: Wake up - thoát Sleep mode
    struct sds011_tx_packet cmd = {0};
    cmd.head        = SDS011_PACKET_HEAD;
    cmd.command     = SDS011_CMD_TX;
    cmd.sub_command = SDS011_TX_CMD_SLEEP_MODE;
    cmd.payload_sleep_mode.method = SDS011_METHOD_SET;
    cmd.payload_sleep_mode.mode   = SDS011_SLEEP_MODE_DISABLED;
    cmd.device_id   = SDS011_ID_ALL;
    cmd.tail        = SDS011_PACKET_TAIL;
    sds011_send_cmd_to_queue(&cmd, pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Da gui lenh WAKE UP");

    vTaskDelay(pdMS_TO_TICKS(500));

    // Lệnh 2: Set Active report mode
    memset(&cmd, 0, sizeof(cmd));
    cmd.head        = SDS011_PACKET_HEAD;
    cmd.command     = SDS011_CMD_TX;
    cmd.sub_command = SDS011_TX_CMD_REPORT_MODE;
    cmd.payload_report_mode.method = SDS011_METHOD_SET;
    cmd.payload_report_mode.mode   = SDS011_REPORT_MODE_ACTIVE;
    cmd.device_id   = SDS011_ID_ALL;
    cmd.tail        = SDS011_PACKET_TAIL;
    sds011_send_cmd_to_queue(&cmd, pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Da gui lenh ACTIVE mode");

    esp_timer_handle_t sensor_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = &sensor_timer_callback,
        .name     = "sensor_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sensor_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_timer, 1000000));

    float pm25 = 0.0f;
    float pm10 = 0.0f;

    while (1)
    {
        xEventGroupWaitBits(s_timer_event_group, TIMER_TRIGGER_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "Sample Time = %.3f s", now / 1000000.0);

        uint16_t bh_lux = 0;
        TEMT6000_illuminance_t temt_lux = 0;

        bh1750_read(&bh1750_dev, &bh_lux);
        temt6000__ReadIlluminance(&temt_sensor, 10, &temt_lux);

        struct sds011_rx_packet dust_packet;
        if (sds011_recv_data_from_queue(&dust_packet, pdMS_TO_TICKS(1500)) == SDS011_OK)
        {
            pm25 = ((dust_packet.payload_query_data.pm2_5_high << 8)
                   | dust_packet.payload_query_data.pm2_5_low) / 10.0f;
            pm10 = ((dust_packet.payload_query_data.pm10_high << 8)
                   | dust_packet.payload_query_data.pm10_low) / 10.0f;
        }

        float lux = (float)bh_lux;
        ESP_LOGI(TAG, "BH=%.2f lx | TEMT=%.2f lx | PM2.5=%.1f | PM10=%.1f",
                 lux, (float)temt_lux, pm25, pm10);

        char line1[32], line2[32], line3[32], line4[32];
        snprintf(line1, sizeof(line1), "BH:%d lx", bh_lux);
        snprintf(line2, sizeof(line2), "TEMT:%.1f", (float)temt_lux);
        snprintf(line3, sizeof(line3), "PM25:%.1f", pm25);
        snprintf(line4, sizeof(line4), "PM10:%.1f", pm10);

        ssd1306_clear_screen(&oled, false);
        ssd1306_display_text(&oled, 0, "LIGHT MONITOR", 14, false);
        ssd1306_display_text(&oled, 1, line1, strlen(line1), false);
        ssd1306_display_text(&oled, 2, line2, strlen(line2), false);
        ssd1306_display_text(&oled, 3, line3, strlen(line3), false);
        ssd1306_display_text(&oled, 4, line4, strlen(line4), false);

        char payload[250];
        snprintf(payload, sizeof(payload),
                 "{\"lux\":%d,\"temt\":%.2f,\"pm25\":%.1f,\"pm10\":%.1f}",
                 bh_lux, (float)temt_lux, pm25, pm10);

        ESP_LOGI(TAG, "SEND: %s", payload);

        esp_mqtt_client_publish(mqtt_client, "v1/devices/me/telemetry",
                                payload, 0, 1, 0);
    }
}