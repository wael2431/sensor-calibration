/* Estimates the BME280 from the DHT11: y = f(a), degree-3 polynomial (Horner) in C.
 * Rust has already validated the active calibration; here we only evaluate it. */
#include "calib.h"
#include "calib_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol.h"
#include "sensor_if.h"
#include "tasks.h"

_Static_assert(SENSOR_CH == PROTO_MEAS_CH, "SENSOR_CH and PROTO_MEAS_CH must match");
_Static_assert(SENSOR_CH == CAL_MAX_CHANNELS, "SENSOR_CH and CAL_MAX_CHANNELS must match");

static const char *TAG = "proc";

typedef struct {
    QueueHandle_t raw_q;
    QueueHandle_t cal_q;
} proc_args_t;

static void processing_task(void *arg)
{
    proc_args_t args = *(proc_args_t *)arg;
    vPortFree(arg);

    uint32_t dropped = 0;
    raw_sample_t s;
    cal_t cal;

    for (;;) {
        if (xQueueReceive(args.raw_q, &s, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        calib_store_get(&cal);      /* active calibration (atomic switch handled by the store) */

        proto_measure_t m = {
            .ts_us = (uint64_t)s.ts_us,
            .cal_rev = cal.revision,
            .a_valid = s.a_valid,
            .ref_valid = s.ref_valid,
        };
        for (int i = 0; i < SENSOR_CH; i++) {
            const uint8_t bit = (uint8_t)(1u << i);
            m.a[i] = (s.a_valid & bit) ? s.a[i] : 0;
            m.ref[i] = (s.ref_valid & bit) ? s.ref[i] : 0;

            if (s.a_valid & bit) {
                int32_t y;
                int32_t r = calib_eval(&cal.ch[i], s.a[i], &y);
                if (r >= 0) {
                    m.est[i] = y;
                    m.est_valid |= bit;
                    if (r == CALIB_EVAL_OOR) {
                        m.est_oor |= bit;
                    }
                }
            }
        }
        if (xQueueSend(args.cal_q, &m, 0) != pdTRUE) {
            if ((++dropped % 100) == 1) {
                ESP_LOGW(TAG, "queue full: %lu measurements lost", (unsigned long)dropped);
            }
        }
    }
}

esp_err_t processing_task_start(QueueHandle_t raw_q, QueueHandle_t cal_q)
{
    proc_args_t *a = pvPortMalloc(sizeof *a);
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    a->raw_q = raw_q;
    a->cal_q = cal_q;
    if (xTaskCreate(processing_task, "proc", 3072, a, 5, NULL) != pdPASS) {
        vPortFree(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
