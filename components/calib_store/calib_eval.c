#include "calib_eval.h"

int32_t calib_eval(const cal_channel_t *ch, int32_t a, int32_t *y)
{
    if (ch == NULL || y == NULL) {
        return CALIB_EVAL_ERR;
    }
    const int64_t span = (int64_t)ch->a_max - (int64_t)ch->a_min;
    if (span <= 0) {
        return CALIB_EVAL_ERR;
    }

    /* Normalized input, clamped to [0, 1]: the polynomial is only reliable over the fitted range. */
    int32_t status = CALIB_EVAL_OK;
    int64_t d = (int64_t)a - (int64_t)ch->a_min;
    if (d < 0) {
        d = 0;
        status = CALIB_EVAL_OOR;
    } else if (d > span) {
        d = span;
        status = CALIB_EVAL_OOR;
    }
    const int64_t x = (d << CAL_Q) / span;          /* 0 .. 65536 */

    /* Horner: 3 multiplications, 3 additions. */
    int64_t r = ch->c[3];
    r = ((r * x) >> CAL_Q) + ch->c[2];
    r = ((r * x) >> CAL_Q) + ch->c[1];
    r = ((r * x) >> CAL_Q) + ch->c[0];

    int64_t v = (r + (1 << (CAL_Q - 1))) >> CAL_Q;   /* round to nearest */
    if (v < ch->y_min) {
        v = ch->y_min;
    } else if (v > ch->y_max) {
        v = ch->y_max;
    }
    *y = (int32_t)v;
    return status;
}
