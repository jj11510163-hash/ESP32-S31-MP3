#include "lcd_font.h"

#include <stddef.h>
#include <string.h>
#include "lcd_gfx.h"

extern const uint8_t lcd18_bin_start[] asm("_binary_lcd18_bin_start");
extern const uint8_t lcd24_bin_start[] asm("_binary_lcd24_bin_start");

typedef struct {
    const uint8_t *base;
    const uint8_t *pixels;
    uint32_t count;
    uint8_t width;
    uint8_t height;
    uint8_t stride;
} font_t;

static font_t s_small, s_large;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static font_t *font_for(int size)
{
    font_t *font = size >= 22 ? &s_large : &s_small;
    if (font->base) return font;
    const uint8_t *base = size >= 22 ? lcd24_bin_start : lcd18_bin_start;
    if (memcmp(base, "S31A", 4) != 0) return NULL;
    font->base = base;
    font->width = base[4];
    font->height = base[5];
    font->stride = base[6];
    font->count = le32(base + 8);
    font->pixels = base + 12 + font->count * 5;
    return font;
}

static uint32_t next_cp(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    if (!*p) return 0;
    uint32_t cp;
    int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2] &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        n = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3] &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               (p[3] & 0xC0) == 0x80) {
        cp = ((p[0] & 7) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        n = 4;
    } else { cp = '?'; n = 1; }
    *s += n;
    return cp;
}

static uint32_t glyph_index(font_t *font, uint32_t cp)
{
    uint32_t lo = 0, hi = font->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t value = le32(font->base + 12 + mid * 5);
        if (value == cp) return mid;
        if (value < cp) lo = mid + 1; else hi = mid;
    }
    if (cp != '?') return glyph_index(font, '?');
    return 0;
}

static int advance(font_t *font, uint32_t index)
{
    return font->base[12 + index * 5 + 4];
}

static void draw_glyph_clip(font_t *font, uint32_t index, int x, int y,
                            uint32_t color, int clip_left, int clip_right)
{
    const uint8_t *data = font->pixels + index * font->height * font->stride;
    for (int gy = 0; gy < font->height; gy++) {
        for (int gx = 0; gx < font->width; gx++) {
            if (x + gx < clip_left || x + gx >= clip_right) continue;
            uint8_t byte = data[gy * font->stride + gx / 4];
            uint8_t alpha = (byte >> (6 - (gx % 4) * 2)) & 3;
            if (alpha) lcd_gfx_pixel_alpha(x + gx, y + gy, color, alpha);
        }
    }
}

static void draw_glyph(font_t *font, uint32_t index, int x, int y, uint32_t color)
{
    draw_glyph_clip(font, index, x, y, color, 0, LCD_W);
}

int lcd_text_height(int size)
{
    font_t *font = font_for(size);
    return font ? font->height : 0;
}

int lcd_text_width(const char *utf8, int size)
{
    font_t *font = font_for(size);
    if (!font || !utf8) return 0;
    int width = 0;
    while (*utf8) width += advance(font, glyph_index(font, next_cp(&utf8)));
    return width;
}

void lcd_text(int x, int y, const char *utf8, int size, uint32_t color)
{
    font_t *font = font_for(size);
    if (!font || !utf8) return;
    while (*utf8) {
        uint32_t index = glyph_index(font, next_cp(&utf8));
        draw_glyph(font, index, x, y, color);
        x += advance(font, index);
    }
}

void lcd_text_ellipsis(int x, int y, int width, const char *utf8, int size, uint32_t color)
{
    font_t *font = font_for(size);
    if (!font || !utf8 || width <= 0) return;
    if (lcd_text_width(utf8, size) <= width) {
        lcd_text(x, y, utf8, size, color);
        return;
    }
    uint32_t dot = glyph_index(font, 0x2026);
    int limit = width - advance(font, dot);
    int used = 0;
    while (*utf8) {
        uint32_t index = glyph_index(font, next_cp(&utf8));
        int step = advance(font, index);
        if (used + step > limit) break;
        draw_glyph(font, index, x + used, y, color);
        used += step;
    }
    draw_glyph(font, dot, x + used, y, color);
}

void lcd_text_center(int x, int y, int width, const char *utf8, int size, uint32_t color)
{
    int text_w = lcd_text_width(utf8, size);
    if (text_w > width) {
        lcd_text_ellipsis(x, y, width, utf8, size, color);
    } else {
        lcd_text(x + (width - text_w) / 2, y, utf8, size, color);
    }
}

void lcd_text_marquee(int x, int y, int width, const char *utf8, int size,
                      uint32_t color, uint32_t scroll_px)
{
    font_t *font = font_for(size);
    if (!font || !utf8 || width <= 0) return;
    int text_w = lcd_text_width(utf8, size);
    if (text_w <= width) {
        lcd_text(x, y, utf8, size, color);
        return;
    }
    const int period = text_w + 36;
    const int offset = (int)(scroll_px % period);
    for (int copy = 0; copy < 2; copy++) {
        int pen = x - offset + copy * period;
        const char *p = utf8;
        while (*p) {
            uint32_t index = glyph_index(font, next_cp(&p));
            int step = advance(font, index);
            if (pen + font->width > x && pen < x + width)
                draw_glyph_clip(font, index, pen, y, color, x, x + width);
            pen += step;
        }
    }
}
