idf_component_register(SRCS "bsp_i2c.c" "bsp_sd.c" "bsp_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_sdmmc esp_driver_gpio fatfs sdmmc led_strip driver)
