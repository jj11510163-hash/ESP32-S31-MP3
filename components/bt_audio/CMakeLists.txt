idf_component_register(SRCS "bt_audio.c" "bt_spp.c" "src/bt_stream.c" "src/aac_enc.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES app_core music bt esp_timer freertos espressif__esp_audio_codec)
