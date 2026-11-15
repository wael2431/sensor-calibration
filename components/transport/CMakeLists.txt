<<<<<<< HEAD
idf_component_register(SRCS "transport_usb.c" "transport.c" "transport_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_driver_usb_serial_jtag bt)
=======
idf_component_register(SRCS "transport.c" "transport_uart.c" "transport_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver bt)
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84
