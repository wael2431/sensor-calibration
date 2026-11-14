#include "sdkconfig.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "transport.h"

#define USB_BUF_SIZE   1024
#define RX_BLOCK       64

static const char *TAG = "tp_usb";
static TaskHandle_t s_task = NULL;
static transport_rx_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

static void usb_rx_task(void *arg)
{
    uint8_t buf[RX_BLOCK];

    for (;;) {
        // Read directly from native USB driver buffer
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        if (n > 0 && s_cb) {
            s_cb(buf, (size_t)n, s_cb_ctx);
        }
    }
}

static esp_err_t usb_tp_open(transport_rx_cb_t cb, void *ctx)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_BUF_SIZE,
        .tx_buffer_size = USB_BUF_SIZE,
    };

    s_cb = cb;
    s_cb_ctx = ctx;

    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        return e;
    }

    if (xTaskCreate(usb_rx_task, "usb_rx_evt", 3072, NULL, 7, &s_task) != pdPASS) {
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB Serial/JTAG transport initialized");
    return ESP_OK;
}

static esp_err_t usb_tp_send(const uint8_t *data, size_t len)
{
    int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
    return (n == (int)len) ? ESP_OK : ESP_FAIL;
}

static void usb_tp_close(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    usb_serial_jtag_driver_uninstall();
    s_cb = NULL;
}

static bool usb_tp_connected(void)
{
    return true;
}

const transport_t transport_usb = {
    .name = "usb",
    .open = usb_tp_open,
    .send = usb_tp_send,
    .close = usb_tp_close,
    .is_connected = usb_tp_connected,
};