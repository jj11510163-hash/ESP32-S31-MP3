#include "lcd_gfx.h"

#include <stdlib.h>

static uint16_t *s_frame;

uint16_t lcd_color(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 16) & 0xF8) << 8) |
                      (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xF8) >> 3));
}

void lcd_gfx_bind(uint16_t *frame)
{
    s_frame = frame;
}

void lcd_gfx_pixel_alpha(int x, int y, uint32_t rgb, uint8_t alpha3)
{
    if (!s_frame || x < 0 || y < 0 || x >= LCD_W || y >= LCD_H || !alpha3) return;
    uint16_t *dest = &s_frame[y * LCD_W + x];
    if (alpha3 >= 3) {
        *dest = lcd_color(rgb);
        return;
    }
    uint16_t old = *dest;
    int old_r = (old >> 11) & 31, old_g = (old >> 5) & 63, old_b = old & 31;
    int new_r = ((rgb >> 16) & 0xFF) >> 3;
    int new_g = ((rgb >> 8) & 0xFF) >> 2;
    int new_b = (rgb & 0xFF) >> 3;
    *dest = (uint16_t)((((old_r * (3 - alpha3) + new_r * alpha3) / 3) << 11) |
                       (((old_g * (3 - alpha3) + new_g * alpha3) / 3) << 5) |
                       ((old_b * (3 - alpha3) + new_b * alpha3) / 3));
}

void lcd_clear(uint32_t rgb)
{
    if (!s_frame) return;
    uint16_t c = lcd_color(rgb);
    for (int i = 0; i < LCD_W * LCD_H; i++) s_frame[i] = c;
}

void lcd_rect(int x, int y, int w, int h, uint32_t rgb)
{
    if (!s_frame || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w;
    int y1 = y + h > LCD_H ? LCD_H : y + h;
    uint16_t c = lcd_color(rgb);
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) s_frame[py * LCD_W + px] = c;
    }
}

void lcd_round_rect(int x, int y, int w, int h, int radius, uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    for (int py = y; py < y + h; py++) {
        int dy = py < y + radius ? y + radius - py - 1 :
                 py >= y + h - radius ? py - (y + h - radius) : 0;
        for (int px = x; px < x + w; px++) {
            int dx = px < x + radius ? x + radius - px - 1 :
                     px >= x + w - radius ? px - (x + w - radius) : 0;
            if (dx * dx + dy * dy <= radius * radius) lcd_gfx_pixel_alpha(px, py, rgb, 3);
        }
    }
}

void lcd_circle(int cx, int cy, int radius, uint32_t rgb)
{
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) lcd_gfx_pixel_alpha(x, y, rgb, 3);
        }
    }
}

void lcd_line(int x0, int y0, int x1, int y1, int thickness, uint32_t rgb)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        lcd_circle(x0, y0, thickness / 2, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void lcd_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t rgb)
{
    int min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x) min_x = x2;
    int max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x) max_x = x2;
    int min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y) min_y = y2;
    int max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y) max_y = y2;
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int a = (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
            int b = (x - x1) * (y2 - y1) - (y - y1) * (x2 - x1);
            int c = (x - x2) * (y0 - y2) - (y - y2) * (x0 - x2);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                lcd_gfx_pixel_alpha(x, y, rgb, 3);
        }
    }
}
