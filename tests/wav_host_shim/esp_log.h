#pragma once

#include <stdio.h>

#define ESP_LOGW(tag, fmt, ...) \
    do { (void)(tag); fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)
