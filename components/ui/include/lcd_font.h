#pragma once

#include <stdint.h>

/* Both sizes share a top-aligned text box and a true common baseline. */
int lcd_text_width(const char *utf8, int size);
int lcd_text_height(int size);
void lcd_text(int x, int y, const char *utf8, int size, uint32_t color);
void lcd_text_ellipsis(int x, int y, int width, const char *utf8, int size, uint32_t color);
void lcd_text_center(int x, int y, int width, const char *utf8, int size, uint32_t color);
void lcd_text_marquee(int x, int y, int width, const char *utf8, int size,
                      uint32_t color, uint32_t scroll_px);
