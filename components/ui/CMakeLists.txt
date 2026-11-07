idf_component_register(SRCS "lcd_panel.c" "lcd_touch.c" "lcd_font.c" "lcd_gfx.c" "lcd_lyrics.c" "lcd_cover.c" "cover_image_decoder.c" "lcd_settings.c" "ui_lcd_home.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "assets/lcd18.bin" "assets/lcd24.bin" "assets/mode_icons.bin"
                       REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_jpeg esp_lcd esp_timer app_core bsp music bt_audio net_service fw_update)
