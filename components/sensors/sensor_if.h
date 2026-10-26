/* Common sensor interface.
 * Each sensor provides SENSOR_CH channels:
 *   channel 0 = temperature, in 0.01 °C
 *   channel 1 = relative humidity, in 0.01 %RH
 * DHT11 = polynomial input `a`; BME280 = reference (collect mode) that we want to estimate.
 */
#ifndef SENSOR_IF_H
#define SENSOR_IF_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_CH 2

typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*read)(int32_t out[SENSOR_CH]);
} sensor_ops_t;

extern const sensor_ops_t sensor_bme280;
extern const sensor_ops_t sensor_dht11;

/* Timestamped raw sample (acquisition -> processing). */
typedef struct {
    int64_t  ts_us;                 /* esp_timer_get_time() */
    uint8_t  a_valid;               /* mask: DHT11 channels read (input) */
    uint8_t  ref_valid;             /* mask: BME280 channels read (reference, collect mode) */
    int32_t  a[SENSOR_CH];          /* DHT11 : polynomial input */
    int32_t  ref[SENSOR_CH];        /* BME280: reference (collect mode) */
} raw_sample_t;

#ifdef __cplusplus
}
#endif

#endif
