/* Application protocol, identical whatever the link (UART, BLE, ...).
 *
 * Raw frame (little-endian):
 *   [type u8][seq u8][len u16][payload: len bytes][crc32 u32]   crc32 over type..payload
 * On the wire: COBS(raw frame) followed by a 0x00 delimiter.
 *
 * Messages:
 *   0x01 MEASURE (device -> host), 40 bytes:
 *        ts_us u64, cal_rev u32,
 *        a_valid u8, est_valid u8, est_oor u8, ref_valid u8      (masks: bit0 = temperature, bit1 = humidity)
 *        a[2] i32    DHT11 values, polynomial input (0.01 °C, 0.01 %RH)
 *        est[2] i32  BME280 estimate = f(a)   (valid if est_valid)
 *        ref[2] i32  measured BME280 (valid if ref_valid; collect mode only)
 *        est_oor: bit set to 1 if `a` was outside the calibration's [a_min, a_max] (input clamped)
 *   0x10 CALIB   (host -> device): calibration blob (see calib.h, 80 bytes)
 *   0x20 ACK     (device -> host): acked_type u8, acked_seq u8, revision u32
 *   0x21 NACK    (device -> host): acked_type u8, acked_seq u8, error i32
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cobs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_MSG_MEASURE   0x01
#define PROTO_MSG_CALIB     0x10
#define PROTO_MSG_ACK       0x20
#define PROTO_MSG_NACK      0x21

#define PROTO_ERR_UNKNOWN_TYPE (-101)

#define PROTO_MAX_PAYLOAD   96
#define PROTO_HDR_LEN       4
#define PROTO_CRC_LEN       4
#define PROTO_RAW_MAX       (PROTO_HDR_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)
#define PROTO_ENC_MAX       (COBS_MAX_ENCODED(PROTO_RAW_MAX) + 1)   /* + delimiter */

#define PROTO_MEAS_CH       2
#define PROTO_MEASURE_LEN   40

typedef struct {
    uint64_t ts_us;
    uint32_t cal_rev;                 /* calibration revision applied */
    uint8_t  a_valid;                 /* DHT11 read (input) */
    uint8_t  est_valid;               /* estimate computed */
    uint8_t  est_oor;                 /* input outside the calibration range (clamped) */
    uint8_t  ref_valid;               /* BME280 read (reference, collect mode) */
    int32_t  a[PROTO_MEAS_CH];
    int32_t  est[PROTO_MEAS_CH];
    int32_t  ref[PROTO_MEAS_CH];
} proto_measure_t;

typedef struct {
    uint8_t        type;
    uint8_t        seq;
    uint16_t       len;
    const uint8_t *payload;    /* only valid during the callback */
} proto_frame_t;

typedef void (*proto_frame_cb_t)(const proto_frame_t *frame, void *ctx);

/* Stream reassembler: one per link. Resynchronizes on the 0x00 delimiter. */
typedef struct {
    uint8_t  buf[PROTO_ENC_MAX];
    size_t   len;
    bool     overflow;
    uint32_t frames_ok;
    uint32_t frames_bad;
} proto_rx_t;

void proto_rx_init(proto_rx_t *rx);
void proto_rx_feed(proto_rx_t *rx, const uint8_t *data, size_t len,
                   proto_frame_cb_t cb, void *ctx);

/* Encoders: return the number of bytes to send (delimiter included), 0 on error.
 * `out` must be at least PROTO_ENC_MAX bytes. */
size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t cap);
size_t proto_encode_measure(uint8_t seq, const proto_measure_t *m, uint8_t *out, size_t cap);
size_t proto_encode_ack(uint8_t seq, uint8_t acked_type, uint8_t acked_seq, uint32_t revision,
                        uint8_t *out, size_t cap);
size_t proto_encode_nack(uint8_t seq, uint8_t acked_type, uint8_t acked_seq, int32_t error,
                         uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
