idf_component_register(SRCS "app_events.c" "settings.c" "input.c" "serial_input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_driver_gpio esp_timer vfs esp_driver_uart bsp music)
