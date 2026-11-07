idf_component_register(SRCS "fw_update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update bootloader_support esp_app_format bsp music net_service)
