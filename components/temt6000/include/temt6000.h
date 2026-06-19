#ifndef TEMT6000_H
#define TEMT6000_H

#include <stdint.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"

typedef enum {
    TEMT6000_OK = 0,
    TEMT6000_INV_ARG,
    TEMT6000_ADC_CONF_FAIL,
    TEMT6000_ADC_OP_FAIL
} TEMT6000_error_t;

typedef float TEMT6000_illuminance_t;
typedef float TEMT6000_intensity_t;

typedef struct TEMT6000_t TEMT6000_t;

struct TEMT6000_t
{
    esp_adc_cal_characteristics_t   adcCharacteristics; 
    adc_channel_t                   channel;            
};

TEMT6000_error_t temt6000__Init(TEMT6000_t * const device, const adc_unit_t unit, const adc_channel_t channel);
TEMT6000_error_t temt6000__ReadIlluminance(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_illuminance_t * const illuminanceOut);
TEMT6000_error_t temt6000__ReadLightIntensity(const TEMT6000_t * const device, const uint32_t samplesNo, TEMT6000_intensity_t * const intensityOut);
TEMT6000_intensity_t temt6000__IlluminanceToLightIntensity(const TEMT6000_illuminance_t illuminance);

#endif /* TEMT6000_H */