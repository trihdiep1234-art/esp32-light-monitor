#include <stdio.h>
#include "temt6000.h"

#define TEMT6000_ADC_WIDTH                          ADC_WIDTH_BIT_12
#define TEMT6000_ADC_ATTEN                          ADC_ATTEN_DB_11

// Đổi từ 3900 về 1100 làm điện áp tham chiếu mặc định chuẩn của ESP32
#define TEMT6000_ADC_ATTEN_VREF                     1100 

#define TEMT6000_PERCENTAGE_CONVERTER_VALUE         100.f
#define TEMT6000_ILLUMINANCE_CONVERTER_VALUE        0.2f
#define TEMT6000_INTENSITY_CONVERTER_VALUE          1000.f

#define TEMT6000_ADC_MARGIN_LOW_REDUCED_VALUE       0
#define TEMT6000_ADC_MARGIN_LOW_MV                  142
#define TEMT6000_SYSTEM_VCC_MV                      3300

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

    int allAdcRaws = 0;
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
        allAdcRaws += adcRaw;
    }

    uint32_t avgRaw = allAdcRaws / samplesNo;
    
    // Giữ lại dòng debug để bạn theo dõi
    printf("[DEBUG] ADC RAW: %lu\n", avgRaw); 

    uint32_t measuredMilliVoltage = 0;
    if (avgRaw == 0) {
        measuredMilliVoltage = 0;
    } else {
        measuredMilliVoltage = esp_adc_cal_raw_to_voltage(avgRaw, &device->adcCharacteristics);
    }

    // --- CẬP NHẬT CÔNG THỨC LOGIC NGƯỢC TỐI ƯU ---
    uint32_t trueMilliVoltage = 0;
    
    // Vì VCC thực tế cấp cho cảm biến thường là 3300mV
    if (measuredMilliVoltage < 3300) {
        // Càng nắng to, measuredMilliVoltage càng về 0 -> trueMilliVoltage càng lớn (gần 3300mV)
        trueMilliVoltage = 3300 - measuredMilliVoltage; 
    } else {
        trueMilliVoltage = 0; 
    }

    // Quy đổi thẳng sang Lux (Không trừ nhiễu quá sâu làm mất dải đo của phòng)
    // Hệ số 0.5f hoặc 0.2f tùy thuộc linh kiện, giữ 0.2f theo thư viện cũ của bạn
    *illuminanceOut = 0.2f * trueMilliVoltage; 

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