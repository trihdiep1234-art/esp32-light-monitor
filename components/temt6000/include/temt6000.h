/*
 * MIT License
 * * Modified for ESP-IDF v4.x and Greenhouse Environmental Monitoring
 */

#ifndef TEMT6000_H_
#define TEMT6000_H_

#include <stdint.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"

// --- CÁC MÃ LỖI (ERROR CODES) ---
typedef enum {
    TEMT6000_OK = 0,
    TEMT6000_INV_ARG,
    TEMT6000_ADC_CONF_FAIL,
    TEMT6000_ADC_OP_FAIL
} TEMT6000_error_t;

// --- KIỂU DỮ LIỆU ĐẦU RA ---
typedef float TEMT6000_illuminance_t; // Độ rọi tuyệt đối (Lux)
typedef float TEMT6000_intensity_t;   // Cường độ ánh sáng tương đối (%)

// --- CẤU TRÚC LƯU TRỮ TRẠNG THÁI CẢM BIẾN ---
typedef struct {
    adc_channel_t channel;
    esp_adc_cal_characteristics_t adcCharacteristics;
} TEMT6000_t;

// --- CÁC HÀM GIAO TIẾP (API) ---

/**
 * @brief Khởi tạo module TEMT6000
 * @param device Con trỏ tới struct thiết bị
 * @param unit ADC_UNIT_1 hoặc ADC_UNIT_2 (Ưu tiên ADC_UNIT_1 trên ESP32)
 * @param channel Kênh ADC nối với chân S (Signal) của cảm biến
 * @return Trạng thái khởi tạo
 */
TEMT6000_error_t temt6000__Init(TEMT6000_t * const device, const adc_unit_t unit, const adc_channel_t channel);

/**
 * @brief Đọc độ rọi lý thuyết (Lux) - Phục vụ tính toán nội bộ hoặc tham khảo
 * @param device Con trỏ tới struct thiết bị
 * @param samplesNo Số lượng mẫu ADC để lấy trung bình (giảm nhiễu)
 * @param illuminanceOut Con trỏ lưu giá trị Lux trả về
 * @return Trạng thái đọc
 */
TEMT6000_error_t temt6000__ReadIlluminance(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_illuminance_t * const illuminanceOut);

/**
 * @brief Đọc cường độ ánh sáng (%) - Dữ liệu chính để đánh giá độ cản sáng của bụi
 * @param device Con trỏ tới struct thiết bị
 * @param samplesNo Số lượng mẫu ADC
 * @param intensityOut Con trỏ lưu giá trị % (0.00% - 100.00%)
 * @return Trạng thái đọc
 */
TEMT6000_error_t temt6000__ReadLightIntensity(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_intensity_t * const intensityOut);

/**
 * @brief Hàm hỗ trợ: Chuyển đổi từ Lux sang %
 */
TEMT6000_intensity_t temt6000__IlluminanceToLightIntensity(const TEMT6000_illuminance_t illuminance);

#endif // TEMT6000_H_