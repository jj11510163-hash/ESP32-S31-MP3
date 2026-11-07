cmake_minimum_required(VERSION 3.16)

# GCC 15 在 IDF 自带的 bluedroid 里对 strncpy 报 -Werror=stringop-truncation（上游代码问题），
# 这里在项目级关掉该警告，避免动 IDF 源码。
add_compile_options(-Wno-stringop-truncation -Wno-error=stringop-truncation)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s31_player_lcd)
