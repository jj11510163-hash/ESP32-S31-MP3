#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char title[64];
    char artist[40];
    char album[40];
    char publisher[48];
    uint16_t year;
    uint16_t track_no;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint64_t total_samples;
    uint32_t lyrics_offset;
    uint32_t lyrics_length;
    uint32_t jpeg_offset;
    uint32_t jpeg_length;
    uint32_t cover_offset;
    uint32_t cover_length;
} flac_tags_t;

/* Reads FLAC metadata only; offsets point into the original file. */
bool flac_tags_read(const char *path, flac_tags_t *tags);
