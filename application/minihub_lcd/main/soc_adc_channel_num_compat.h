#pragma once

/* IDF 6.2 removed SOC_ADC_CHANNEL_NUM(). esp_board_manager 0.5.15 still
 * calls it. On esp32s31 both ADC units have 8 channels
 * (hal/adc_ll.h ADC_LL_CHANNEL_NUM). */
#ifndef SOC_ADC_CHANNEL_NUM
#define SOC_ADC_CHANNEL_NUM(unit) (8)
#endif
