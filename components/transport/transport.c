#include "transport.h"

#include "sdkconfig.h"

static const transport_t *const s_list[] = {
#if CONFIG_APP_TRANSPORT_UART
    &transport_uart,
#endif
#if CONFIG_APP_TRANSPORT_BLE
    &transport_ble,
#endif
    NULL,
};

const transport_t *const *transport_list(size_t *count)
{
    if (count) {
        *count = sizeof s_list / sizeof s_list[0] - 1;
    }
    return s_list;
}
