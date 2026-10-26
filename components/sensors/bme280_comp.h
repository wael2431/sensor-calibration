/* BME280 compensation (integer formulas from the Bosch datasheet). Plain C, testable on a PC. */
#ifndef BME280_COMP_H
#define BME280_COMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t T1;
    int16_t  T2, T3;
    uint8_t  H1;
    int16_t  H2;
    uint8_t  H3;
    int16_t  H4, H5;
    int8_t   H6;
} bme280_trim_t;

/* Decodes registers 0x88..0xA1 (26 bytes) and 0xE1..0xE7 (7 bytes). */
void bme280_parse_trim(const uint8_t calib1[26], const uint8_t calib2[7], bme280_trim_t *t);

/* Temperature in 0.01 °C; updates t_fine (needed for humidity). */
int32_t bme280_comp_temp(int32_t adc_T, const bme280_trim_t *t, int32_t *t_fine);

/* Humidity in 0.01 %RH (clamped to 0..100 %). */
int32_t bme280_comp_hum(int32_t adc_H, int32_t t_fine, const bme280_trim_t *t);

#ifdef __cplusplus
}
#endif

#endif
