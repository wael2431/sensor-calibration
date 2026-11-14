/* Evaluator for the calibration polynomial (plain C, no ESP-IDF dependency: testable on a PC).
 *
 *   x = clamp((a - a_min) / (a_max - a_min), 0, 1)       Q16.16
 *   y = c0 + x*(c1 + x*(c2 + x*c3))                      Horner, Q16.16, 64-bit intermediates
 *   y = clamp(round(y), y_min, y_max)
 *
 * The 64-bit intermediates rule out any overflow: libfixmath is not needed.
 * The structure has already been validated by Rust (cal_parse) before it gets here.
 */
#ifndef CALIB_EVAL_H
#define CALIB_EVAL_H

#include <stdint.h>

#include "calib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CALIB_EVAL_OK    0
#define CALIB_EVAL_OOR   1      /* `a` outside [a_min, a_max]: input clamped, result is flat beyond the range */
#define CALIB_EVAL_ERR  (-1)    /* invalid parameters */

int32_t calib_eval(const cal_channel_t *ch, int32_t a, int32_t *y);

#ifdef __cplusplus
}
#endif

#endif
