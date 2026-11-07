#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Locate embedded JPEG or PNG data within an audio file. */
bool music_cover_find(const char *path, uint32_t *offset, uint32_t *length);
