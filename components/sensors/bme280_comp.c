#include "bme280_comp.h"

static int16_t s16(uint8_t lsb, uint8_t msb) { return (int16_t)(((uint16_t)msb << 8) | lsb); }

void bme280_parse_trim(const uint8_t c1[26], const uint8_t c2[7], bme280_trim_t *t)
{
    t->T1 = (uint16_t)(((uint16_t)c1[1] << 8) | c1[0]);
    t->T2 = s16(c1[2], c1[3]);
    t->T3 = s16(c1[4], c1[5]);
    t->H1 = c1[25];                                   /* 0xA1 */
    t->H2 = s16(c2[0], c2[1]);                        /* 0xE1, 0xE2 */
    t->H3 = c2[2];                                    /* 0xE3 */
    t->H4 = (int16_t)(((int16_t)(int8_t)c2[3] * 16) | (c2[4] & 0x0F));   /* 0xE4, 0xE5[3:0] */
    t->H5 = (int16_t)(((int16_t)(int8_t)c2[5] * 16) | (c2[4] >> 4));     /* 0xE6, 0xE5[7:4] */
    t->H6 = (int8_t)c2[6];                            /* 0xE7 */
}

int32_t bme280_comp_temp(int32_t adc_T, const bme280_trim_t *t, int32_t *t_fine)
{
    int32_t var1 = (((adc_T >> 3) - ((int32_t)t->T1 << 1)) * (int32_t)t->T2) >> 11;
    int32_t d = (adc_T >> 4) - (int32_t)t->T1;
    int32_t var2 = (((d * d) >> 12) * (int32_t)t->T3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

int32_t bme280_comp_hum(int32_t adc_H, int32_t t_fine, const bme280_trim_t *t)
{
    int32_t v = t_fine - 76800;

    v = (((((adc_H << 14) - ((int32_t)t->H4 << 20) - ((int32_t)t->H5 * v)) + 16384) >> 15) *
         (((((((v * (int32_t)t->H6) >> 10) * (((v * (int32_t)t->H3) >> 11) + 32768)) >> 10) + 2097152) *
           (int32_t)t->H2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)t->H1) >> 4);
    v = (v < 0) ? 0 : v;
    v = (v > 419430400) ? 419430400 : v;

    uint32_t h = (uint32_t)(v >> 12);                 /* %RH in Q22.10 */
    return (int32_t)((h * 25u) >> 8);                 /* -> 0.01 %RH  (h/1024*100) */
}
