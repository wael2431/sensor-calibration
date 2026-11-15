<<<<<<< HEAD
idf_component_register(
    SRCS "acquisition_task.c" "processing_task.c" "comm_task.c"
    INCLUDE_DIRS "."
    REQUIRES sensors protocol calib_store transport calib_rs
    PRIV_REQUIRES esp_timer
)
=======
idf_component_register(SRCS "acquisition_task.c" "processing_task.c" "comm_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES sensors protocol calib_store transport calib_rs)
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84
