#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char title[64];
    char artist[40];
    char album[40];
    uint32_t duration_ms;
    uint16_t year;
    uint16_t track_no;
    uint32_t cover_offset;
    uint32_t cover_length;
} m4a_tags_t;

bool m4a_tags_read(const char *path, m4a_tags_t *tags);
