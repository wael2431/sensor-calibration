#include "sdkconfig.h"

#if CONFIG_APP_TRANSPORT_UART

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "transport.h"

#define UART_PORT       ((uart_port_t)CONFIG_APP_UART_NUM)
#define UART_BUF_SIZE   1024
#define UART_EVT_DEPTH  20
#define RX_BLOCK        64

static const char *TAG = "tp_uart";
static QueueHandle_t s_evt_q;
static TaskHandle_t s_task;
static transport_rx_cb_t s_cb;
static void *s_cb_ctx;

static void uart_event_task(void *arg)
{
    uart_event_t ev;
    uint8_t buf[RX_BLOCK];

    for (;;) {
        if (xQueueReceive(s_evt_q, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case UART_DATA: {
            size_t left = ev.size;
            while (left > 0) {
                int n = uart_read_bytes(UART_PORT, buf, left > RX_BLOCK ? RX_BLOCK : left, 0);
                if (n <= 0) {
                    break;
                }
                left -= (size_t)n;
                if (s_cb) {
                    s_cb(buf, (size_t)n, s_cb_ctx);
                }
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "RX overflow: flushing (COBS resynchronizes at the next 0x00)");
            uart_flush_input(UART_PORT);
            xQueueReset(s_evt_q);
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            ESP_LOGW(TAG, "line error (%d)", ev.type);
            break;
        default:
            break;
        }
    }
}

static esp_err_t uart_tp_open(transport_rx_cb_t cb, void *ctx)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_APP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    s_cb = cb;
    s_cb_ctx = ctx;

    esp_err_t e = uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, UART_EVT_DEPTH, &s_evt_q, 0);
    if (e != ESP_OK) {
        return e;
    }
    e = uart_param_config(UART_PORT, &cfg);
    if (e == ESP_OK) {
        e = uart_set_pin(UART_PORT, CONFIG_APP_UART_TX_GPIO, CONFIG_APP_UART_RX_GPIO,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (e != ESP_OK) {
        uart_driver_delete(UART_PORT);
        return e;
    }
    if (xTaskCreate(uart_event_task, "uart_evt", 3072, NULL, 7, &s_task) != pdPASS) {
        uart_driver_delete(UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART%d open: %d baud, TX=%d RX=%d", CONFIG_APP_UART_NUM,
             CONFIG_APP_UART_BAUD, CONFIG_APP_UART_TX_GPIO, CONFIG_APP_UART_RX_GPIO);
    return ESP_OK;
}

static esp_err_t uart_tp_send(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(UART_PORT, data, len);
    return (n == (int)len) ? ESP_OK : ESP_FAIL;
}

static void uart_tp_close(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    uart_driver_delete(UART_PORT);
    s_cb = NULL;
}

static bool uart_tp_connected(void)
{
    return true;
}

const transport_t transport_uart = {
    .name = "uart",
    .open = uart_tp_open,
    .send = uart_tp_send,
    .close = uart_tp_close,
    .is_connected = uart_tp_connected,
};

#endif /* CONFIG_APP_TRANSPORT_UART */
