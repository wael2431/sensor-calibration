idf_component_register(SRCS "calib_store.c" "calib_eval.c"
                       INCLUDE_DIRS "."
                       REQUIRES calib_rs
                       PRIV_REQUIRES nvs_flash)
