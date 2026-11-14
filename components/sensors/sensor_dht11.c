/* DHT11: 1-wire protocol, bit-banging timed with esp_timer. Polynomial input.
 * Output: temperature 0.01 °C, humidity 0.01 %RH (actual resolution: 1 °C / 1 %RH).
 *
 * - A read takes ~25 ms; the sensor's minimum period is 1 s.
 * - The ~4 ms needed to read the 40 bits run with interrupts masked: if BLE is active,
 *   prefer a long sampling period (APP_ACQ_PERIOD_MS) or the RMT peripheral.
 * - An external 4.7..10 kΩ pull-up to 3V3 is recommended.
 */
#include "sdkconfig.h"
#include "sensor_if.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN ((gpio_num_t)CONFIG_SENSOR_DHT11_GPIO)

static const char *TAG = "dht11";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_us = -2000000;

/* Waits for the line to reach `level`. Returns the wait time in µs, or -1 (timeout). */
static int wait_level(int level, int timeout_us)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t dt;
    while (gpio_get_level(PIN) != level) {
        dt = esp_timer_get_time() - t0;
        if (dt > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - t0);
}

static esp_err_t dht_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_SENSOR_DHT11_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e == ESP_OK) {
        gpio_set_level(PIN, 1);
        ESP_LOGI(TAG, "DHT11 on GPIO%d", CONFIG_SENSOR_DHT11_GPIO);
    }
    return e;
}

static esp_err_t dht_read(int32_t out[SENSOR_CH])
{
    uint8_t data[5] = { 0 };
    const int64_t now = esp_timer_get_time();

    if (now - s_last_us < 900000) {
        return ESP_ERR_NOT_FINISHED;                /* the DHT11 needs ~1 s between two reads */
    }
    s_last_us = now;

    /* Start signal: line low for >= 18 ms (outside the critical section). */
    gpio_set_level(PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t e = ESP_OK;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(PIN, 1);                         /* release the line */
    if (wait_level(0, 100) < 0 ||                   /* response: low ~80 µs */
        wait_level(1, 100) < 0 ||                   /*           high ~80 µs */
        wait_level(0, 100) < 0) {
        e = ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < 40 && e == ESP_OK; i++) {
        if (wait_level(1, 80) < 0) {                /* end of the low phase (~50 µs) */
            e = ESP_ERR_TIMEOUT;
            break;
        }
        const int high = wait_level(0, 120);        /* high duration: ~27 µs = 0, ~70 µs = 1 */
        if (high < 0) {
            e = ESP_ERR_TIMEOUT;
            break;
        }
        data[i / 8] = (uint8_t)((data[i / 8] << 1) | (high > 45 ? 1 : 0));
    }
    portEXIT_CRITICAL(&s_mux);

    if (e != ESP_OK) {
        return e;
    }
    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    int32_t t10 = (int32_t)(data[2] & 0x7F) * 10 + (data[3] & 0x7F);   /* 0.1 °C */
    if (data[3] & 0x80) {
        t10 = -t10;
    }
    out[0] = t10 * 10;                              /* -> 0.01 °C */
    out[1] = ((int32_t)data[0] * 10 + data[1]) * 10; /* -> 0.01 %RH */
    return ESP_OK;
}

const sensor_ops_t sensor_dht11 = { .name = "DHT11", .init = dht_init, .read = dht_read };
