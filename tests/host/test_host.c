/* C tests on the PC: COBS, protocol, BME280 compensation, Horner evaluator, and Rust interface (validation).
 * Usage: test_host vectors.txt blob.bin measure_out.bin */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bme280_comp.h"
#include "calib.h"
#include "calib_eval.h"
#include "protocol.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

_Static_assert(sizeof(cal_channel_t) == 32, "layout canal");
_Static_assert(sizeof(cal_t) == 72, "layout cal_t");

/* ------------------------------------------------------------------ COBS */
static void test_cobs(void)
{
    srand(1);
    for (int it = 0; it < 20000; it++) {
        uint8_t in[600], enc[700], dec[700];
        size_t len = (size_t)(rand() % 600);
        int mode = rand() % 3;
        for (size_t i = 0; i < len; i++) in[i] = mode == 0 ? rand() & 0xFF : mode == 1 ? (rand() % 8 ? 0x55 : 0) : 0x77;
        size_t n = cobs_encode(in, len, enc, sizeof enc);
        CHECK(n > 0, "encode len=%zu", len);
        for (size_t i = 0; i < n; i++) if (enc[i] == 0) { CHECK(0, "zero byte in the encoding"); return; }
        int d = cobs_decode(enc, n, dec, sizeof dec);
        if (d != (int)len || memcmp(dec, in, len)) { CHECK(0, "round trip len=%zu d=%d", len, d); return; }
    }
    printf("cobs: 20000-case fuzz OK\n");
}

/* -------------------------------------------------------------- protocol */
static int g_got; static uint8_t g_type, g_seq; static uint16_t g_len; static uint8_t g_pl[PROTO_MAX_PAYLOAD];
static void on_frame(const proto_frame_t *f, void *c)
{
    (void)c; g_got++; g_type = f->type; g_seq = f->seq; g_len = f->len; memcpy(g_pl, f->payload, f->len);
}

static void test_protocol(const char *measure_out)
{
    proto_measure_t m = { .ts_us = 1234567, .cal_rev = 3, .a_valid = 3, .est_valid = 3, .est_oor = 1, .ref_valid = 3,
                          .a = { 2150, 5500 }, .est = { 2160, 5480 }, .ref = { 2200, 5400 } };
    uint8_t out[PROTO_ENC_MAX];
    size_t n = proto_encode_measure(9, &m, out, sizeof out);
    CHECK(n > 0 && out[n - 1] == 0, "encode measure");

    proto_rx_t rx; proto_rx_init(&rx);
    uint8_t noise[] = { 1, 2, 3, 0, 0, 0xFF, 0 };                /* noise, then resynchronization */
    proto_rx_feed(&rx, noise, sizeof noise, on_frame, NULL);
    for (size_t i = 0; i < n; i++) proto_rx_feed(&rx, &out[i], 1, on_frame, NULL);   /* byte by byte */
    CHECK(g_got == 1 && g_type == PROTO_MSG_MEASURE && g_seq == 9 && g_len == PROTO_MEASURE_LEN,
          "measure decoding got=%d type=%02x len=%u", g_got, g_type, g_len);
    CHECK(rx.frames_bad == 2, "invalid frames counted: %u", rx.frames_bad);

    if (measure_out) {
        FILE *f = fopen(measure_out, "wb");
        if (f) { fwrite(out, 1, n, f); fclose(f); }
    }

    /* maximum payload and corrupted frame */
    uint8_t big[PROTO_MAX_PAYLOAD]; memset(big, 0xA5, sizeof big);
    n = proto_encode(PROTO_MSG_CALIB, 1, big, sizeof big, out, sizeof out);
    CHECK(n > 0, "encode payload max");
    CHECK(proto_encode(PROTO_MSG_CALIB, 1, big, PROTO_MAX_PAYLOAD + 1, out, sizeof out) == 0, "oversized payload rejected");
    out[3] ^= 0x01;
    g_got = 0; proto_rx_init(&rx);
    proto_rx_feed(&rx, out, n, on_frame, NULL);
    CHECK(g_got == 0 && rx.frames_bad == 1, "corrupted frame rejected");

    CHECK(proto_encode_ack(1, 0x10, 2, 5, out, sizeof out) > 0, "ack");
    CHECK(proto_encode_nack(1, 0x10, 2, -5, out, sizeof out) > 0, "nack");
    printf("protocol: OK\n");
}

/* ----------------------------------------------------------------- BME280 */
static void test_bme280(void)
{
    /* Bosch datasheet example (BMP280/BME280): 519888 -> 25.08 °C */
    bme280_trim_t t = { .T1 = 27504, .T2 = 26435, .T3 = -1000 };
    int32_t tf;
    int32_t temp = bme280_comp_temp(519888, &t, &tf);
    CHECK(temp == 2508, "temperature: %d (expected 2508)", temp);

    /* decoding of the humidity trim values */
    uint8_t c1[26] = { 0 }; c1[25] = 75;
    uint8_t c2[7] = { 0x6E, 0x01, 0x00, 0x13, 0x32, 0x03, 0x1E };
    bme280_trim_t p; bme280_parse_trim(c1, c2, &p);
    CHECK(p.H1 == 75 && p.H2 == 366 && p.H3 == 0 && p.H4 == 0x132 && p.H5 == 0x33 && p.H6 == 30,
          "H trims: %d %d %d %d %d %d", p.H1, p.H2, p.H3, p.H4, p.H5, p.H6);

    /* humidity: monotonic and within [0, 10000] */
    int32_t prev = -1;
    for (int adc = 15000; adc <= 45000; adc += 500) {
        int32_t h = bme280_comp_hum(adc, 128000, &p);
        CHECK(h >= 0 && h <= 10000 && h >= prev, "humidity adc=%d h=%d prev=%d", adc, h, prev);
        prev = h;
    }
    printf("bme280: temperature matches the Bosch example, humidity is consistent\n");
}

/* --------------------------------------------- evaluator + Rust validation */
static void test_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { CHECK(0, "opening %s", path); return; }

    int a_min, a_max, y_min, y_max, c[4], valid, n;
    int cases = 0, valid_cases = 0, evals = 0;
    while (fscanf(f, " CASE %d %d %d %d %d %d %d %d %d %d", &a_min, &a_max, &y_min, &y_max,
                  &c[0], &c[1], &c[2], &c[3], &valid, &n) == 10) {
        cal_t cal; cal_default(&cal);
        cal.ch[0] = (cal_channel_t){ a_min, a_max, y_min, y_max, { c[0], c[1], c[2], c[3] } };
        int32_t r = cal_validate(&cal);                          /* Rust validation */
        CHECK((r == CAL_OK) == (valid == 1), "case %d: Rust validation=%d, expected valid=%d", cases, r, valid);
        cases++;
        valid_cases += valid;
        for (int i = 0; i < n; i++) {
            int a, ey, eoor;
            if (fscanf(f, "%d %d %d", &a, &ey, &eoor) != 3) { CHECK(0, "truncated file"); fclose(f); return; }
            int32_t y = 0;
            int32_t s = calib_eval(&cal.ch[0], a, &y);           /* C evaluation (Horner) */
            evals++;
            if (s < 0 || y != ey || (s == CALIB_EVAL_OOR) != (eoor == 1)) {
                CHECK(0, "case %d a=%d: C y=%d s=%d; Python y=%d oor=%d", cases - 1, a, y, s, ey, eoor);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
    CHECK(cases > 100 && valid_cases > 50, "too few cases (%d, %d valid)", cases, valid_cases);
    printf("calib_eval (C) == Python, Rust validation == Python: %d cases (%d valid), %d evaluations\n",
           cases, valid_cases, evals);
}

static void test_blob(const char *path)
{
    uint8_t blob[CAL_BLOB_LEN + 8];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(blob, 1, sizeof blob, f) : 0;
    if (f) fclose(f);
    CHECK(n == CAL_BLOB_LEN, "Python blob size: %zu", n);

    cal_t c;
    CHECK(cal_parse(blob, n, &c) == CAL_OK, "Python blob accepted by Rust");
    CHECK(c.revision == 42 && c.n_channels == 2, "header (rev=%u)", c.revision);

    uint8_t again[CAL_BLOB_LEN];
    CHECK(cal_serialize(&c, again, sizeof again) == CAL_BLOB_LEN && memcmp(again, blob, n) == 0,
          "Rust re-serialization identical to the Python blob");

    uint8_t bad[CAL_BLOB_LEN];
    memcpy(bad, blob, n); bad[20] ^= 1;
    CHECK(cal_parse(bad, n, &c) == CAL_ERR_CRC, "corrupted bit -> CRC");
    CHECK(cal_parse(blob, n - 1, &c) == CAL_ERR_LEN, "length");
    printf("Python blob <-> Rust: OK\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s vectors.txt blob.bin [measure_out.bin]\n", argv[0]); return 2; }
    test_cobs();
    test_protocol(argc > 3 ? argv[3] : NULL);
    test_bme280();
    test_vectors(argv[1]);
    test_blob(argv[2]);
    if (g_fail) { printf("\n%d FAILURE(S)\n", g_fail); return 1; }
    printf("\nAll C tests pass.\n");
    return 0;
}
