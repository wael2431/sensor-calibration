#include "cobs.h"

#include <string.h>

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (cap < COBS_MAX_ENCODED(len)) {
        return 0;
    }
    size_t read = 0;
    size_t write = 1;
    size_t code_idx = 0;
    uint8_t code = 1;

    while (read < len) {
        if (in[read] == 0) {
            out[code_idx] = code;
            code = 1;
            code_idx = write++;
            read++;
        } else {
            out[write++] = in[read++];
            code++;
            if (code == 0xFF) {
                out[code_idx] = code;
                code = 1;
                code_idx = write++;
            }
        }
    }
    out[code_idx] = code;
    return write;
}

int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t r = 0;
    size_t w = 0;

    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) {
            return -1;
        }
        size_t n = (size_t)code - 1;
        if (r + n > len || w + n > cap) {
            return -1;
        }
        memcpy(out + w, in + r, n);
        w += n;
        r += n;
        if (code != 0xFF && r < len) {
            if (w >= cap) {
                return -1;
            }
            out[w++] = 0;
        }
    }
    return (int)w;
}
