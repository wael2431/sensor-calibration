#include <stdlib.h>

#include "calib_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "sdkconfig.h"
#include "sensor_if.h"
#include "tasks.h"

static const char *TAG = "main";

void app_main(void)
{
    /* NVS: persistent calibration (and PHY data for BLE). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Calibration: reloaded at boot, otherwise default calibration (identity). */
    ESP_ERROR_CHECK(calib_store_init());

    /* Wiring: acquisition --raw_q--> processing --cal_q--> communication */
    QueueHandle_t raw_q = xQueueCreate(CONFIG_APP_QUEUE_RAW_LEN, sizeof(raw_sample_t));
    QueueHandle_t cal_q = xQueueCreate(CONFIG_APP_QUEUE_CAL_LEN, sizeof(proto_measure_t));
    if (raw_q == NULL || cal_q == NULL) {
        ESP_LOGE(TAG, "not enough memory for the queues");
        abort();
    }

    ESP_ERROR_CHECK(comm_task_start(cal_q));
    ESP_ERROR_CHECK(processing_task_start(raw_q, cal_q));
    ESP_ERROR_CHECK(acquisition_task_start(raw_q));

    ESP_LOGI(TAG, "startup complete (period %d ms)", CONFIG_APP_ACQ_PERIOD_MS);
}
