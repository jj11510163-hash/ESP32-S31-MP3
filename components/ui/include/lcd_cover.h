#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LCD_COVER_THUMB_SIZE 48
#define LCD_COVER_THUMB_SLOTS 4

/* Optional JPEG/PNG artwork from a matching file, embedded tag, or album folder. */
void lcd_cover_load(const char *track_path);
bool lcd_cover_available(void);
const uint16_t *lcd_cover_pixels(void);

/* Decode row thumbnails in the background, one at a time. */
bool lcd_cover_thumb_init(void);
bool lcd_cover_thumb_prepare(int slot, const char *track_path);
bool lcd_cover_thumb_take_update(void);
const uint16_t *lcd_cover_thumb_pixels(int slot, const char *track_path);
void lcd_cover_thumb_clear(void);
