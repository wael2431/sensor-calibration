/* Active calibration + NVS persistence.
 * A new calibration only becomes active if it is valid (Rust) AND written to NVS. */
#ifndef CALIB_STORE_H
#define CALIB_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "calib.h"
#include "calib_eval.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CALIB_STORE_ERR_NVS (-100)

/* Call after nvs_flash_init(). Loads the NVS calibration (re-validated by Rust),
 * otherwise uses the default calibration (identity). Only returns an error when out of memory. */
esp_err_t calib_store_init(void);

/* Thread-safe copy of the active calibration. */
void calib_store_get(cal_t *out);

/* Validates (Rust), persists (NVS), then switches over.
 * Returns CAL_OK (0), a CAL_ERR_* code (<0) or CALIB_STORE_ERR_NVS. On failure the old calibration stays active. */
int32_t calib_store_apply_blob(const uint8_t *blob, size_t len);

#ifdef __cplusplus
}
#endif

#endif
