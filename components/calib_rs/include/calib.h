/* C interface of the Rust calibration validator (calib_rs).
 * Keep in sync with src/lib.rs (or regenerate with cbindgen).
 *
 * Model: for each channel (0 = temperature, 1 = humidity), the BME280 value is estimated
 * from the DHT11 value by a degree-3 polynomial on a normalized input:
 *
 *     x = (a - a_min) / (a_max - a_min)          x in [0,1], Q16.16 format
 *     y = c0 + c1*x + c2*x^2 + c3*x^3            (Horner), c[k] in Q16.16
 *     y clamped to [y_min, y_max]
 *
 * All quantities (a, y) are in hundredths of a unit: 0.01 °C or 0.01 %RH.
 *
 * Rust ONLY validates/decodes: evaluation runs in C (calib_store/calib_eval.c).
 *
 * Blob format (little-endian, 80 bytes):
 *   0  u32  magic    = 0x324C4143 ("CAL2")
 *   4  u8   version  = 2
 *   5  u8   channel count = 2
 *   6  u16  reserved (0)
 *   8  u32  revision
 *  12  channel 0: i32 a_min, a_max, y_min, y_max, c0, c1, c2, c3   (32 bytes)
 *  44  channel 1: same
 *  76  u32  CRC32 (IEEE) of bytes 0..75
 */
#ifndef CALIB_H
#define CALIB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAL_MAGIC          0x324C4143u
#define CAL_VERSION        2
#define CAL_MAX_CHANNELS   2
#define CAL_POLY_COEFFS    4
#define CAL_BLOB_LEN       80
#define CAL_Q              16          /* fractional bits of x and of c[k] */

#define CAL_OK              0
#define CAL_ERR_NULL       (-1)
#define CAL_ERR_LEN        (-2)
#define CAL_ERR_MAGIC      (-3)
#define CAL_ERR_VERSION    (-4)
#define CAL_ERR_CRC        (-5)
#define CAL_ERR_CHANNELS   (-6)
#define CAL_ERR_RANGE      (-7)        /* a_min >= a_max, y_min >= y_max, span too large */
#define CAL_ERR_DIVERGES   (-8)        /* the polynomial strays far outside the bounds over [0,1] */

typedef struct cal_channel {
    int32_t a_min;
    int32_t a_max;
    int32_t y_min;
    int32_t y_max;
    int32_t c[CAL_POLY_COEFFS];
} cal_channel_t;

typedef struct cal {
    uint32_t      revision;
    uint8_t       n_channels;
    uint8_t       _pad[3];
    cal_channel_t ch[CAL_MAX_CHANNELS];
} cal_t;

/* Neutral calibration: estimated BME280 = DHT11 value (identity polynomial). */
void     cal_default(cal_t *out);
/* Decodes + validates a blob. Returns CAL_OK or a CAL_ERR_* code. `out` is only written on success. */
int32_t  cal_parse(const uint8_t *buf, size_t len, cal_t *out);
int32_t  cal_validate(const cal_t *cal);
/* Encodes to blob format. Returns the number of bytes written (CAL_BLOB_LEN), or 0 on error. */
size_t   cal_serialize(const cal_t *cal, uint8_t *buf, size_t cap);
/* CRC32 IEEE 802.3 (poly 0xEDB88320), shared with the protocol layer. */
uint32_t cal_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CALIB_H */
