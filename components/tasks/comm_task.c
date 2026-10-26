#include <string.h>

#include "calib_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol.h"
#include "sdkconfig.h"
#include "tasks.h"
#include "transport.h"

#define RX_CHUNK        64
#define MAX_TRANSPORTS  4

static const char *TAG = "comm";

typedef struct {
    uint8_t tid;
    uint8_t len;
    uint8_t data[RX_CHUNK];
} rx_chunk_t;

static QueueHandle_t s_rx_q;
static const transport_t *const *s_tp;
static size_t s_ntp;
static proto_rx_t s_rx[MAX_TRANSPORTS];
static uint8_t s_tx_seq;
static volatile uint32_t s_rx_dropped;

/* Context: driver task (UART event task, NimBLE host): we only copy into the queue. */
static void on_rx_bytes(const uint8_t *data, size_t len, void *ctx)
{
    uint8_t tid = (uint8_t)(uintptr_t)ctx;

    while (len > 0) {
        rx_chunk_t c = { .tid = tid };
        size_t n = len > RX_CHUNK ? RX_CHUNK : len;
        c.len = (uint8_t)n;
        memcpy(c.data, data, n);
        if (xQueueSend(s_rx_q, &c, 0) != pdTRUE) {
            s_rx_dropped++;          /* corrupted frame -> bad CRC -> resynchronization */
            return;
        }
        data += n;
        len -= n;
    }
}

static void reply(uint8_t tid, const uint8_t *buf, size_t n)
{
    if (n > 0 && s_tp[tid]->send(buf, n) != ESP_OK) {
        ESP_LOGW(TAG, "[%s] failed to send the reply", s_tp[tid]->name);
    }
}

static void on_frame(const proto_frame_t *f, void *ctx)
{
    uint8_t tid = (uint8_t)(uintptr_t)ctx;
    uint8_t out[PROTO_ENC_MAX];
    size_t n;

    switch (f->type) {
    case PROTO_MSG_CALIB: {
        int32_t r = calib_store_apply_blob(f->payload, f->len);   /* Rust validation + NVS + switch over */
        if (r == CAL_OK) {
            cal_t c;
            calib_store_get(&c);
            n = proto_encode_ack(s_tx_seq++, f->type, f->seq, c.revision, out, sizeof out);
            ESP_LOGI(TAG, "[%s] calibration accepted (rev. %lu)", s_tp[tid]->name, (unsigned long)c.revision);
        } else {
            n = proto_encode_nack(s_tx_seq++, f->type, f->seq, r, out, sizeof out);
            ESP_LOGW(TAG, "[%s] calibration rejected (%ld)", s_tp[tid]->name, (long)r);
        }
        break;
    }
    default:
        n = proto_encode_nack(s_tx_seq++, f->type, f->seq, PROTO_ERR_UNKNOWN_TYPE, out, sizeof out);
        break;
    }
    reply(tid, out, n);
}

static void send_measure(const proto_measure_t *m)
{
    uint8_t out[PROTO_ENC_MAX];
    size_t n = proto_encode_measure(s_tx_seq++, m, out, sizeof out);

    if (n == 0) {
        return;
    }
    for (size_t i = 0; i < s_ntp; i++) {
        if (s_tp[i]->is_connected()) {
            (void)s_tp[i]->send(out, n);
        }
    }
}

static void comm_task(void *arg)
{
    QueueHandle_t cal_q = (QueueHandle_t)arg;

    QueueSetHandle_t set = xQueueCreateSet(CONFIG_APP_QUEUE_RX_LEN + CONFIG_APP_QUEUE_CAL_LEN);
    BaseType_t ok = (set != NULL) && (xQueueAddToSet(s_rx_q, set) == pdPASS)
                    && (xQueueAddToSet(cal_q, set) == pdPASS);
    if (!ok) {
        ESP_LOGE(TAG, "cannot create the queue set");
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_bad = 0;

    for (;;) {
        /* One item handled per wake-up: keeps the queue-set count consistent with the queues. */
        QueueSetMemberHandle_t m = xQueueSelectFromSet(set, portMAX_DELAY);

        if (m == s_rx_q) {
            rx_chunk_t c;
            if (xQueueReceive(s_rx_q, &c, 0) == pdTRUE && c.tid < s_ntp) {
                proto_rx_feed(&s_rx[c.tid], c.data, c.len, on_frame, (void *)(uintptr_t)c.tid);
                if (s_rx[c.tid].frames_bad != last_bad) {
                    last_bad = s_rx[c.tid].frames_bad;
                    ESP_LOGW(TAG, "[%s] invalid frames: %lu (RX dropped: %lu)", s_tp[c.tid]->name,
                             (unsigned long)last_bad, (unsigned long)s_rx_dropped);
                }
            }
        } else if (m == cal_q) {
            proto_measure_t mm;
            if (xQueueReceive(cal_q, &mm, 0) == pdTRUE) {
                send_measure(&mm);
            }
        }
    }
}

esp_err_t comm_task_start(QueueHandle_t cal_q)
{
    s_tp = transport_list(&s_ntp);
    if (s_ntp == 0 || s_ntp > MAX_TRANSPORTS) {
        ESP_LOGE(TAG, "no transport enabled (Kconfig)");
        return ESP_ERR_INVALID_STATE;
    }
    s_rx_q = xQueueCreate(CONFIG_APP_QUEUE_RX_LEN, sizeof(rx_chunk_t));
    if (s_rx_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_ntp; i++) {
        proto_rx_init(&s_rx[i]);
        esp_err_t e = s_tp[i]->open(on_rx_bytes, (void *)(uintptr_t)i);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "opening transport %s: %s", s_tp[i]->name, esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(TAG, "transport %s active", s_tp[i]->name);
    }
    return xTaskCreate(comm_task, "comm", 4096, cal_q, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
