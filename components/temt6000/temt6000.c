#include <stdio.h>
#include "temt6000.h"

#define TEMT6000_ADC_WIDTH                          ADC_WIDTH_BIT_12
#define TEMT6000_ADC_ATTEN                          ADC_ATTEN_DB_11
#define TEMT6000_ADC_ATTEN_VREF                     1100 

#define TEMT6000_PERCENTAGE_CONVERTER_VALUE         100.f
#define TEMT6000_INTENSITY_CONVERTER_VALUE          1000.f

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

    // Đổi sang uint32_t để chứa được tổng lớn nếu số lần lấy mẫu (samplesNo) cao, chống tràn biến.
    uint32_t allAdcRaws = 0; 
    int adcRaw;

    for (uint32_t i = 0; i < samplesNo; ++i)
    {
        if (ADC_UNIT_1 == device->adcCharacteristics.adc_num) {
            adcRaw = adc1_get_raw((adc1_channel_t) device->channel);
        } else {
            if (ESP_OK != adc2_get_raw((adc2_channel_t) device->channel, TEMT6000_ADC_WIDTH, &adcRaw)) {
                return TEMT6000_ADC_OP_FAIL;
            }
        }
        allAdcRaws += (uint32_t)adcRaw;
    }

    // Tính trung bình
    uint32_t avgRaw = allAdcRaws / samplesNo;

    // Chuyển đổi ADC thô sang Mili-Volt dựa trên Calibration của ESP32
    uint32_t measuredMilliVoltage = 0;
    if (avgRaw > 0) {
        measuredMilliVoltage = esp_adc_cal_raw_to_voltage(avgRaw, &device->adcCharacteristics);
    }

    // =========================================================================
    // CÔNG THỨC ĐÃ ĐƯỢC FIX LỖI:
    // Cảm biến TEMT6000 trả về điện áp tỷ lệ thuận với độ sáng.
    // Công thức chuẩn cho module có trở kéo xuống 10k: Lux = Điện áp (mV) * 0.2
    // =========================================================================
    *illuminanceOut = (float)measuredMilliVoltage * 0.2f; 

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
    // Quy đổi 1000 Lux tương đương 100% cường độ
    float percent = illuminance / TEMT6000_INTENSITY_CONVERTER_VALUE * TEMT6000_PERCENTAGE_CONVERTER_VALUE;
    if(percent > 100.0f) percent = 100.0f;
    if(percent < 0.0f) percent = 0.0f;
    return percent;
}