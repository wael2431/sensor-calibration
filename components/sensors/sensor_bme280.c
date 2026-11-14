/* BME280 over I2C, normal mode (T x1, H x1, pressure disabled).
 * Output: temperature 0.01 °C, humidity 0.01 %RH.
 * Polynomial reference: only used in collect mode (APP_COLLECT_REF). */
#include "sdkconfig.h"
#include "sensor_if.h"

#if CONFIG_APP_COLLECT_REF

#include "bme280_comp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_ID          0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_STATUS      0xF3
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_DATA        0xF7      /* press(3) temp(3) hum(2) */
#define REG_CALIB1      0x88      /* 26 bytes  */
#define REG_CALIB2      0xE1      /* 7 bytes   */
#define CHIP_ID_BME280  0x60      /* 0x58 = BMP280 (no humidity) */

static const char *TAG = "bme280";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bme280_trim_t s_trim;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

static esp_err_t bme_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_SENSOR_BME280_SDA,
        .scl_io_num = CONFIG_SENSOR_BME280_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(e));
        return e;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_SENSOR_BME280_ADDR,
        .scl_speed_hz = CONFIG_SENSOR_BME280_FREQ,
    };
    e = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (e != ESP_OK) {
        goto fail_bus;
    }

    uint8_t id = 0;
    e = rd(REG_ID, &id, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no response at 0x%02x: %s", CONFIG_SENSOR_BME280_ADDR, esp_err_to_name(e));
        goto fail_dev;
    }
    if (id != CHIP_ID_BME280) {
        ESP_LOGE(TAG, "unexpected ID 0x%02x (expected 0x60: BME280, not BMP280)", id);
        e = ESP_ERR_NOT_SUPPORTED;
        goto fail_dev;
    }

    (void)wr(REG_RESET, 0xB6);                      /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 10; i++) {                  /* wait for the trim values to finish loading */
        uint8_t st = 0;
        if (rd(REG_STATUS, &st, 1) == ESP_OK && (st & 0x01) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t c1[26], c2[7];
    e = rd(REG_CALIB1, c1, sizeof c1);
    if (e == ESP_OK) {
        e = rd(REG_CALIB2, c2, sizeof c2);
    }
    if (e != ESP_OK) {
        goto fail_dev;
    }
    bme280_parse_trim(c1, c2, &s_trim);

    /* humidity x1 (applied when ctrl_meas is written), standby 62.5 ms, filter off,
     * temperature x1, pressure disabled, normal mode. */
    e = wr(REG_CTRL_HUM, 0x01);
    if (e == ESP_OK) e = wr(REG_CONFIG, 0x20);
    if (e == ESP_OK) e = wr(REG_CTRL_MEAS, (0x01 << 5) | (0x00 << 2) | 0x03);
    if (e != ESP_OK) {
        goto fail_dev;
    }
    ESP_LOGI(TAG, "BME280 ready (0x%02x)", CONFIG_SENSOR_BME280_ADDR);
    return ESP_OK;

fail_dev:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
fail_bus:
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    s_dev = NULL;
    return e;
}

static esp_err_t bme_read(int32_t out[SENSOR_CH])
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t d[8];
    esp_err_t e = rd(REG_DATA, d, sizeof d);
    if (e != ESP_OK) {
        return e;
    }
    const int32_t adc_t = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
    const int32_t adc_h = ((int32_t)d[6] << 8) | d[7];
    if (adc_t == 0x80000 || adc_h == 0x8000) {
        return ESP_ERR_INVALID_RESPONSE;            /* measurement not available yet */
    }
    int32_t t_fine;
    out[0] = bme280_comp_temp(adc_t, &s_trim, &t_fine);
    out[1] = bme280_comp_hum(adc_h, t_fine, &s_trim);
    return ESP_OK;
}

#else  /* production: the BME280 is not used */

static esp_err_t bme_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t bme_read(int32_t out[SENSOR_CH]) { (void)out; return ESP_ERR_NOT_SUPPORTED; }

#endif

const sensor_ops_t sensor_bme280 = { .name = "BME280", .init = bme_init, .read = bme_read };
