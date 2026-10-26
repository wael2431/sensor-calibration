#include "protocol.h"

#include <string.h>

#include "calib.h"   /* cal_crc32 (Rust): a single CRC implementation */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ transmit */

size_t proto_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t cap)
{
    uint8_t raw[PROTO_RAW_MAX];

    if (len > PROTO_MAX_PAYLOAD || (len > 0 && payload == NULL) || cap < PROTO_ENC_MAX) {
        return 0;
    }
    raw[0] = type;
    raw[1] = seq;
    put_u16(&raw[2], len);
    if (len > 0) {
        memcpy(&raw[PROTO_HDR_LEN], payload, len);
    }
    size_t body = PROTO_HDR_LEN + len;
    put_u32(&raw[body], cal_crc32(raw, body));

    size_t n = cobs_encode(raw, body + PROTO_CRC_LEN, out, cap - 1);
    if (n == 0) {
        return 0;
    }
    out[n] = 0x00;
    return n + 1;
}

size_t proto_encode_measure(uint8_t seq, const proto_measure_t *m, uint8_t *out, size_t cap)
{
    uint8_t p[PROTO_MEASURE_LEN];

    if (m == NULL) {
        return 0;
    }
    put_u64(&p[0], m->ts_us);
    put_u32(&p[8], m->cal_rev);
    p[12] = m->a_valid;
    p[13] = m->est_valid;
    p[14] = m->est_oor;
    p[15] = m->ref_valid;
    for (int i = 0; i < PROTO_MEAS_CH; i++) {
        put_u32(&p[16 + 4 * i], (uint32_t)m->a[i]);
        put_u32(&p[24 + 4 * i], (uint32_t)m->est[i]);
        put_u32(&p[32 + 4 * i], (uint32_t)m->ref[i]);
    }
    return proto_encode(PROTO_MSG_MEASURE, seq, p, sizeof p, out, cap);
}

size_t proto_encode_ack(uint8_t seq, uint8_t acked_type, uint8_t acked_seq, uint32_t revision,
                        uint8_t *out, size_t cap)
{
    uint8_t p[6] = { acked_type, acked_seq };
    put_u32(&p[2], revision);
    return proto_encode(PROTO_MSG_ACK, seq, p, sizeof p, out, cap);
}

size_t proto_encode_nack(uint8_t seq, uint8_t acked_type, uint8_t acked_seq, int32_t error,
                         uint8_t *out, size_t cap)
{
    uint8_t p[6] = { acked_type, acked_seq };
    put_u32(&p[2], (uint32_t)error);
    return proto_encode(PROTO_MSG_NACK, seq, p, sizeof p, out, cap);
}

/* ------------------------------------------------------------------ receive */

void proto_rx_init(proto_rx_t *rx)
{
    memset(rx, 0, sizeof *rx);
}

static void handle_block(proto_rx_t *rx, proto_frame_cb_t cb, void *ctx)
{
    uint8_t raw[PROTO_RAW_MAX];
    int n = cobs_decode(rx->buf, rx->len, raw, sizeof raw);

    if (n < PROTO_HDR_LEN + PROTO_CRC_LEN) {
        rx->frames_bad++;
        return;
    }
    uint16_t plen = get_u16(&raw[2]);
    if (plen > PROTO_MAX_PAYLOAD || (size_t)n != (size_t)PROTO_HDR_LEN + plen + PROTO_CRC_LEN) {
        rx->frames_bad++;
        return;
    }
    size_t body = PROTO_HDR_LEN + plen;
    if (cal_crc32(raw, body) != get_u32(&raw[body])) {
        rx->frames_bad++;
        return;
    }
    rx->frames_ok++;
    if (cb) {
        proto_frame_t f = { .type = raw[0], .seq = raw[1], .len = plen, .payload = &raw[PROTO_HDR_LEN] };
        cb(&f, ctx);
    }
}

void proto_rx_feed(proto_rx_t *rx, const uint8_t *data, size_t len,
                   proto_frame_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00) {                       /* end of frame / resynchronization */
            if (rx->overflow) {
                rx->frames_bad++;
            } else if (rx->len > 0) {
                handle_block(rx, cb, ctx);
            }
            rx->len = 0;
            rx->overflow = false;
        } else if (rx->len < sizeof rx->buf) {
            rx->buf[rx->len++] = b;
        } else {
            rx->overflow = true;
        }
    }
}
