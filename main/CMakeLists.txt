idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_core bsp music bt_audio ui net_service app_update)
