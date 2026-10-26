/* Periodic read of the DHT11 (polynomial input) and, in collect mode, of the BME280 (reference).
 * The DHT11 sets the pace (>= 1 s, ~25 ms blocking read). The BME280 is read right after:
 * each sample then holds a nearly simultaneous (a, ref) pair, usable by the fit. */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensor_if.h"
#include "tasks.h"

#define ALL_CH ((uint8_t)((1u << SENSOR_CH) - 1u))

static const char *TAG = "acq";

static void acquisition_task(void *arg)
{
    QueueHandle_t raw_q = (QueueHandle_t)arg;
    uint32_t dropped = 0;
    uint32_t dht_err = 0;

    esp_err_t e = sensor_dht11.init();
    const bool dht_ok = (e == ESP_OK);
    ESP_LOGI(TAG, "sensor %s (input): %s", sensor_dht11.name, dht_ok ? "OK" : esp_err_to_name(e));

#if CONFIG_APP_COLLECT_REF
    e = sensor_bme280.init();
    const bool bme_ok = (e == ESP_OK);
    ESP_LOGI(TAG, "sensor %s (reference): %s", sensor_bme280.name, bme_ok ? "OK" : esp_err_to_name(e));
#endif

    TickType_t period = pdMS_TO_TICKS(CONFIG_APP_ACQ_PERIOD_MS);
    if (period == 0) {
        period = 1;
    }
    TickType_t last = xTaskGetTickCount();     /* the DHT11 needs ~1 s after power-up */

    for (;;) {
        vTaskDelayUntil(&last, period);

        raw_sample_t s = { .ts_us = esp_timer_get_time() };

        if (dht_ok) {
            esp_err_t r = sensor_dht11.read(s.a);            /* ~25 ms, blocking */
            if (r == ESP_OK) {
                s.a_valid = ALL_CH;
            } else if ((++dht_err % 10) == 1) {
                ESP_LOGW(TAG, "DHT11 read failed: %s (%lu)", esp_err_to_name(r), (unsigned long)dht_err);
            }
        }

#if CONFIG_APP_COLLECT_REF
        if (bme_ok && sensor_bme280.read(s.ref) == ESP_OK) {
            s.ref_valid = ALL_CH;
        }
#endif

        if (xQueueSend(raw_q, &s, 0) != pdTRUE) {
            if ((++dropped % 100) == 1) {
                ESP_LOGW(TAG, "queue full: %lu samples lost", (unsigned long)dropped);
            }
        }
    }
}

esp_err_t acquisition_task_start(QueueHandle_t raw_q)
{
    return xTaskCreate(acquisition_task, "acq", 3072, raw_q, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
