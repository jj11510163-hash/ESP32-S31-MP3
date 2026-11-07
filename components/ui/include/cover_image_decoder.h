#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Software fallback for JPEG (including progressive) and PNG artwork. */
bool cover_image_decode_software(const uint8_t *encoded, size_t bytes,
                                 uint16_t *target, int target_w, int target_h);
