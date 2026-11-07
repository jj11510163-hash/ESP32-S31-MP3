idf_component_register(SRCS "net_service.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES app_core bsp music esp_wifi esp_netif esp_http_server esp_timer nvs_flash)
