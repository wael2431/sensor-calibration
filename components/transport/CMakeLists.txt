idf_component_register(SRCS "transport_usb.c" "transport.c" "transport_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_driver_usb_serial_jtag bt)
