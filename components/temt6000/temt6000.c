#include <stdio.h>
#include <stdlib.h> // Thư viện cần thiết cho hàm qsort
#include "temt6000.h"
#include "esp_log.h"

static const char *TEMT6000_TAG = "TEMT6000";

// Giá trị raw tối đa của ADC 12-bit (2^12 - 1). Khi raw chạm/gần mức này,
// điện áp ngõ ra cảm biến đã vượt quá dải đo được của ADC (bão hoà phần
// cứng) -- giá trị lux tính ra sẽ bị "kẹp cứng", KHÔNG phản ánh đúng ánh
// sáng thực tế nữa.
#define TEMT6000_ADC_RAW_MAX 4095

#define TEMT6000_ADC_WIDTH                          ADC_WIDTH_BIT_12
#define TEMT6000_ADC_ATTEN                          ADC_ATTEN_DB_11
#define TEMT6000_ADC_ATTEN_VREF                     1100 

#define TEMT6000_PERCENTAGE_CONVERTER_VALUE         100.f
#define TEMT6000_INTENSITY_CONVERTER_VALUE          1000.f

// --- HÀM HỖ TRỢ CHO THUẬT TOÁN SẮP XẾP ---
static int compare_ints(const void* a, const void* b) {
    // QUAN TRỌNG: phải dereference (*) để lấy GIÁ TRỊ int mà con trỏ trỏ tới.
    // Bản cũ ép thẳng con trỏ (const int)a/b sang int => so sánh ĐỊA CHỈ
    // bộ nhớ thay vì giá trị ADC, khiến qsort không sắp xếp đúng theo giá
    // trị => bước "cắt 20% hai đầu lọc nhiễu" phía dưới không có tác dụng.
    int arg1 = *(const int*)a;
    int arg2 = *(const int*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

TEMT6000_error_t temt6000__Init(TEMT6000_t * const device, const adc_unit_t unit, const adc_channel_t channel)
{
    if (!device) return TEMT6000_INV_ARG;

    if (ADC_UNIT_1 == unit)
    {
        if (ESP_OK != adc1_config_width(TEMT6000_ADC_WIDTH)) return TEMT6000_ADC_CONF_FAIL;
        if (ESP_OK != adc1_config_channel_atten((adc1_channel_t) channel, TEMT6000_ADC_ATTEN)) return TEMT6000_ADC_CONF_FAIL;
    }
    else
    {
        if (ESP_OK != adc2_config_channel_atten((adc2_channel_t) channel, TEMT6000_ADC_ATTEN)) return TEMT6000_ADC_CONF_FAIL;
    }

    esp_adc_cal_characterize(unit, TEMT6000_ADC_ATTEN, TEMT6000_ADC_WIDTH, TEMT6000_ADC_ATTEN_VREF, &device->adcCharacteristics);
    device->channel = channel;

    return TEMT6000_OK;
}

TEMT6000_error_t temt6000__ReadIlluminance(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_illuminance_t * const illuminanceOut)
{
    if (!device || !illuminanceOut || samplesNo == 0) return TEMT6000_INV_ARG;

    // Giới hạn max 100 mẫu để tránh tốn quá nhiều RAM của ESP32
    uint32_t actualSamples = samplesNo > 100 ? 100 : samplesNo;
    int raw_array[100] = {0}; 

    // 1. LẤY MẪU VÀO MẢNG
    for (uint32_t i = 0; i < actualSamples; ++i)
    {
        if (ADC_UNIT_1 == device->adcCharacteristics.adc_num) {
            raw_array[i] = adc1_get_raw((adc1_channel_t) device->channel);
        } else {
            if (ESP_OK != adc2_get_raw((adc2_channel_t) device->channel, TEMT6000_ADC_WIDTH, &raw_array[i])) {
                return TEMT6000_ADC_OP_FAIL;
            }
        }
    }

    // 2. LỌC NHIỄU MEDIAN
    // Sắp xếp mảng từ nhỏ đến lớn
    qsort(raw_array, actualSamples, sizeof(int), compare_ints);
    
    // Tính toán số lượng mẫu cần cắt bỏ ở mỗi đầu (20%)
    int trim_count = actualSamples * 0.2; 
    uint32_t sumRaws = 0;
    uint32_t validSamples = 0;

    // Chỉ tính tổng các phần tử ở khúc giữa, bỏ qua các xung nhiễu đột biến ở 2 đầu mảng
    for (int i = trim_count; i < (actualSamples - trim_count); i++) {
        sumRaws += raw_array[i];
        validSamples++;
    }

    // 3. TÍNH TRUNG BÌNH LÕI
    uint32_t avgRaw = validSamples > 0 ? (sumRaws / validSamples) : 0;

    // 4. CHUYỂN ĐỔI SANG LUX
    uint32_t measuredMilliVoltage = 0;
    if (avgRaw > 0) {
        measuredMilliVoltage = esp_adc_cal_raw_to_voltage(avgRaw, &device->adcCharacteristics);
    }

    // Áp dụng khoảng trừ nhiễu điện áp nền tối (giống datasheet của Vishay TEMT6000)
    // Nếu điện áp dưới 142mV coi như tối hoàn toàn để màn hình không bị nhảy số lẻ tẻ.
    if (measuredMilliVoltage <= 142) {
        measuredMilliVoltage = 0;
    }

    // Công thức tính chuẩn theo module 10k: Lux = Điện áp (mV) * 0.2
    *illuminanceOut = (float)measuredMilliVoltage * 0.2f; 

    // ================= PHÁT HIỆN BÃO HOÀ ADC =================
    // Nếu giá trị raw trung bình gần/đạt mức tối đa (4095), nghĩa là điện
    // áp ngõ ra cảm biến đã vượt quá dải đo của ADC -- giá trị lux ở trên
    // chỉ là mức TRẦN của phần cứng, không phải ánh sáng thực tế đo được.
    if (avgRaw >= (TEMT6000_ADC_RAW_MAX - 5)) {
        ESP_LOGW(TEMT6000_TAG,
                 "ADC BAO HOA (raw=%lu/%d, mV=%lu) -> lux=%.1f la GIOI HAN PHAN CUNG, KHONG phai gia tri sang thuc te!",
                 (unsigned long)avgRaw, TEMT6000_ADC_RAW_MAX,
                 (unsigned long)measuredMilliVoltage, *illuminanceOut);
    }

    return TEMT6000_OK;
}

TEMT6000_error_t temt6000__ReadLightIntensity(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_intensity_t * const intensityOut)
{
    if (!intensityOut) return TEMT6000_INV_ARG;

    TEMT6000_illuminance_t illuminance;
    TEMT6000_error_t status = temt6000__ReadIlluminance(device, samplesNo, &illuminance);
    if (TEMT6000_OK != status) return status;

    *intensityOut = temt6000__IlluminanceToLightIntensity(illuminance);

    return TEMT6000_OK;
}

TEMT6000_intensity_t temt6000__IlluminanceToLightIntensity(const TEMT6000_illuminance_t illuminance)
{
    float percent = illuminance / TEMT6000_INTENSITY_CONVERTER_VALUE * TEMT6000_PERCENTAGE_CONVERTER_VALUE;
    if(percent > 100.0f) percent = 100.0f;
    if(percent < 0.0f) percent = 0.0f;
    return percent;
}