#pragma once

#include <stdint.h>
#include "lcd_panel.h"

/* All coordinates and colors are shared with tools/render_home_preview.py. */
#define C_BG       0xF7F8F3
#define C_CARD     0xFFFFFF
#define C_INK      0x203236
#define C_MUTED    0x66787A
#define C_TEAL     0x287D78
#define C_SOFT     0xE3F0ED
#define C_BORDER   0xDFE7E3
#define C_COVER_A  0xD1E9E3
#define C_COVER_B  0xA6CEC3

void lcd_gfx_bind(uint16_t *frame);
uint16_t lcd_color(uint32_t rgb);
void lcd_clear(uint32_t rgb);
void lcd_rect(int x, int y, int w, int h, uint32_t rgb);
void lcd_round_rect(int x, int y, int w, int h, int radius, uint32_t rgb);
void lcd_circle(int cx, int cy, int radius, uint32_t rgb);
void lcd_line(int x0, int y0, int x1, int y1, int thickness, uint32_t rgb);
void lcd_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t rgb);
void lcd_gfx_pixel_alpha(int x, int y, uint32_t rgb, uint8_t alpha3);
