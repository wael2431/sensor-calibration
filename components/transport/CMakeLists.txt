idf_component_register(SRCS "transport.c" "transport_uart.c" "transport_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver bt)
