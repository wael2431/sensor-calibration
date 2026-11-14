/* BLE transport: "Nordic UART"-style GATT service (NUS), NimBLE.
 *   RX (write, host -> device): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX (notification, device -> host): 6E400003-...
 * The byte stream is the same as on UART (COBS frames): frames may be split over
 * several writes/notifications, the `protocol` reassembler stitches them back together. */
#include "sdkconfig.h"

#if CONFIG_APP_TRANSPORT_BLE

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "transport.h"

#define BLE_RX_MAX 512

static const char *TAG = "tp_ble";

static const ble_uuid128_t UUID_SVC =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_RX =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_TX =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static transport_rx_cb_t s_cb;
static void *s_cb_ctx;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static volatile uint16_t s_mtu = 23;
static volatile bool s_subscribed;
static uint8_t s_rx_buf[BLE_RX_MAX];

static void start_advertising(void);

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t n = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof s_rx_buf, &n);
        if (rc != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (s_cb && n > 0) {
            s_cb(s_rx_buf, n, s_cb_ctx);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_RX.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &UUID_TX.u,
                .access_cb = gatt_access_cb,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle %u)", s_conn);
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_mtu = 23;
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notifications %s", s_subscribed ? "enabled" : "disabled");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU = %u", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = (uint8_t)strlen(name);
    f.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adresse BLE rc=%d", rc);
        return;
    }
    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_tp_open(transport_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        return e;
    }
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT rc=%d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(CONFIG_APP_BLE_DEVICE_NAME);
    ble_att_set_preferred_mtu(CONFIG_APP_BLE_MTU);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static esp_err_t ble_tp_send(const uint8_t *data, size_t len)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t chunk = (s_mtu > 23 ? s_mtu : 23) - 3;
    size_t off = 0;

    while (off < len) {
        size_t n = len - off < chunk ? len - off : chunk;
        int rc = BLE_HS_ENOMEM;
        for (int retry = 0; retry < 20 && rc == BLE_HS_ENOMEM; retry++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
            if (om == NULL) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);   /* takes ownership of om */
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        if (rc != 0) {
            return ESP_FAIL;
        }
        off += n;
    }
    return ESP_OK;
}

static void ble_tp_close(void)
{
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    s_cb = NULL;
    s_subscribed = false;
}

static bool ble_tp_connected(void)
{
    return s_subscribed && s_conn != BLE_HS_CONN_HANDLE_NONE;
}

const transport_t transport_ble = {
    .name = "ble",
    .open = ble_tp_open,
    .send = ble_tp_send,
    .close = ble_tp_close,
    .is_connected = ble_tp_connected,
};

#endif /* CONFIG_APP_TRANSPORT_BLE */
