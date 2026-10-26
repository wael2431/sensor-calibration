#ifndef TASKS_H
#define TASKS_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* raw_q: raw_sample_t items    (sensor_if.h)
 * cal_q: proto_measure_t items  (protocol.h) */
esp_err_t acquisition_task_start(QueueHandle_t raw_q);
esp_err_t processing_task_start(QueueHandle_t raw_q, QueueHandle_t cal_q);
esp_err_t comm_task_start(QueueHandle_t cal_q);

#ifdef __cplusplus
}
#endif

#endif
