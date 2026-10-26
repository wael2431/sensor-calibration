#ifndef COBS_H
#define COBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum output size for `len` input bytes (excluding the 0x00 delimiter). */
#define COBS_MAX_ENCODED(len) ((len) + (len) / 254 + 1)

/* Encodes `in` (no trailing delimiter). Returns the encoded size, or 0 if `cap` is too small. */
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Decodes a COBS block (without the trailing 0x00). Returns the decoded size, or -1 if invalid. */
int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
