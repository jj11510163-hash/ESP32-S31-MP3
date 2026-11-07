idf_component_register(SRCS "music_lib.c" "music_favorites.c" "flac_tags.c" "m4a_tags.c" "music_cover.c" "player.c" "mp3_dec.c"
                            "src/decoder.c" "src/dec_mp3.c"
                            "src/dec_flac.c" "src/dec_wav.c" "src/dec_aac.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src" "third_party"
                       REQUIRES app_core bsp esp_app_format esp_timer freertos esp_driver_sdmmc fatfs espressif__esp_audio_codec)
