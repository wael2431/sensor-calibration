#include "calib_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "calib_store";
static const char *NVS_NS = "calib";
static const char *NVS_KEY = "blob";

static cal_t s_active;
static SemaphoreHandle_t s_lock;

esp_err_t calib_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cal_default(&s_active);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no calibration in NVS: using default calibration (identity)");
        return ESP_OK;
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s: using default calibration", esp_err_to_name(e));
        return ESP_OK;
    }

    uint8_t blob[CAL_BLOB_LEN];
    size_t sz = sizeof blob;
    e = nvs_get_blob(h, NVS_KEY, blob, &sz);
    nvs_close(h);

    if (e == ESP_OK) {
        cal_t c;
        int32_t r = cal_parse(blob, sz, &c);     /* an old format (other size/version) is rejected here */
        if (r == CAL_OK) {
            s_active = c;
            ESP_LOGI(TAG, "calibration reloaded (revision %lu)", (unsigned long)c.revision);
        } else {
            ESP_LOGW(TAG, "invalid NVS calibration (%ld): using default calibration", (long)r);
        }
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_blob: %s: using default calibration", esp_err_to_name(e));
    }
    return ESP_OK;
}

void calib_store_get(cal_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_active;
    xSemaphoreGive(s_lock);
}

int32_t calib_store_apply_blob(const uint8_t *blob, size_t len)
{
    cal_t c;
    int32_t r = cal_parse(blob, len, &c);
    if (r != CAL_OK) {
        return r;
    }

    /* Store the canonical form (re-serialized), not the received bytes. */
    uint8_t canon[CAL_BLOB_LEN];
    size_t n = cal_serialize(&c, canon, sizeof canon);
    if (n == 0) {
        return CAL_ERR_RANGE;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(e));
        return CALIB_STORE_ERR_NVS;
    }
    e = nvs_set_blob(h, NVS_KEY, canon, n);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS write: %s", esp_err_to_name(e));
        return CALIB_STORE_ERR_NVS;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active = c;                     /* switch over */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "new calibration active (revision %lu)", (unsigned long)c.revision);
    return CAL_OK;
}
