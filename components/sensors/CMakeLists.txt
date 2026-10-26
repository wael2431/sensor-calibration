idf_component_register(SRCS "sensor_bme280.c" "bme280_comp.c" "sensor_dht11.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_timer)
