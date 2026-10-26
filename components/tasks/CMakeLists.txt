idf_component_register(SRCS "acquisition_task.c" "processing_task.c" "comm_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES sensors protocol calib_store transport calib_rs)
