<<<<<<< HEAD
idf_component_register(

    SRCS "sensor_bme280.c" "bme280_comp.c" "sensor_dht11.c"

    INCLUDE_DIRS "."

    PRIV_REQUIRES driver esp_driver_i2c esp_driver_gpio esp_timer

)
=======
idf_component_register(SRCS "sensor_bme280.c" "bme280_comp.c" "sensor_dht11.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_timer)
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84
