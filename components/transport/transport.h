/* Common interface for communication links (UART, BLE, Wi-Fi...).
 * A transport is a byte stream: splitting it into frames is done by `protocol` (COBS). */
#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the transport for each block of received bytes (context: driver task, must not block). */
typedef void (*transport_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

typedef struct {
    const char *name;
    esp_err_t (*open)(transport_rx_cb_t cb, void *ctx);
    esp_err_t (*send)(const uint8_t *data, size_t len);
    void      (*close)(void);
    bool      (*is_connected)(void);   /* link ready to transmit (UART: always true; BLE: subscribed) */
} transport_t;

extern const transport_t transport_uart;
extern const transport_t transport_ble;

/* List of enabled transports (Kconfig), NULL-terminated. `count` is optional. */
const transport_t *const *transport_list(size_t *count);

#ifdef __cplusplus
}
#endif

#endif
