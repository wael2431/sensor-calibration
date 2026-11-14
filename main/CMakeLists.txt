idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES tasks sensors protocol calib_store nvs_flash)
