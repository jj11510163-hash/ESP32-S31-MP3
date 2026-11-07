#include "ui_gfx.h"

#include <math.h>
#include <string.h>

#include "ui_cjk.h"
#include "ui_font_data.h"

/* ------------------------------------------------------------------ */
/* 字体表                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint16_t *data;
    uint8_t w;
    uint8_t h;
} font_desc_t;

static const font_desc_t s_fonts[UI_FONT_MAX] = {
    [UI_FONT_SMALL] = {&ui_font_6x10[0][0], UI_FONT_6X10_W, UI_FONT_6X10_H},
    [UI_FONT_MED] = {&ui_font_7x12[0][0], UI_FONT_7X12_W, UI_FONT_7X12_H},
    [UI_FONT_BIG] = {&ui_font_12x20[0][0], UI_FONT_12X20_W, UI_FONT_12X20_H},
};

#define GLYPH_FIRST 32
#define GLYPH_LAST 126

int ui_font_w(ui_font_t font)
{
    return font < UI_FONT_MAX ? s_fonts[font].w : 6;
}

int ui_font_h(ui_font_t font)
{
    return font < UI_FONT_MAX ? s_fonts[font].h : 10;
}

int ui_text_h(ui_font_t font)
{
    return font == UI_FONT_MED ? 14 : font == UI_FONT_BIG ? 20 : 12;
}

/* ------------------------------------------------------------------ */
/* UTF-8 解码 + CJK 尺寸                                               */
/* ------------------------------------------------------------------ */
static int utf8_next(const char **ps, uint32_t *out)
{
    const uint8_t *s = (const uint8_t *)*ps;
    if (*s == 0) {
        return 0;
    }
    int len = 1;
    uint32_t cp = s[0];
    if (s[0] >= 0xF0 && s[1] && s[2] && s[3]) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        len = 4;
    } else if (s[0] >= 0xE0 && s[1] && s[2]) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        len = 3;
    } else if (s[0] >= 0xC0 && s[1]) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        len = 2;
    } else if (s[0] >= 0x80) {
        cp = '?';
    }
    *ps += len;
    *out = cp;
    return len;
}

static uint8_t cjk_px_for_font(ui_font_t font)
{
    return (font == UI_FONT_BIG) ? 16 : 12;
}

/* ------------------------------------------------------------------ */
/* 画布与裁剪                                                          */
/* ------------------------------------------------------------------ */
#define CLIP_STACK_MAX 6
static int16_t s_clip_stack[CLIP_STACK_MAX][4];
static int s_clip_depth;

void ui_canvas_bind(ui_canvas_t *c, uint8_t *buf)
{
    c->buf = buf;
    c->invert = false;
    ui_clip_reset(c);
}

void ui_clip_reset(ui_canvas_t *c)
{
    c->clip_x0 = 0;
    c->clip_y0 = 0;
    c->clip_x1 = UI_W;
    c->clip_y1 = UI_H;
    s_clip_depth = 0;
}

void ui_clip_set(ui_canvas_t *c, int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > UI_W) {
        x1 = UI_W;
    }
    if (y1 > UI_H) {
        y1 = UI_H;
    }
    c->clip_x0 = (int16_t)x0;
    c->clip_y0 = (int16_t)y0;
    c->clip_x1 = (int16_t)(x1 < x0 ? x0 : x1);
    c->clip_y1 = (int16_t)(y1 < y0 ? y0 : y1);
}

void ui_clip_push_rect(ui_canvas_t *c, int x, int y, int w, int h)
{
    if (s_clip_depth < CLIP_STACK_MAX) {
        s_clip_stack[s_clip_depth][0] = c->clip_x0;
        s_clip_stack[s_clip_depth][1] = c->clip_y0;
        s_clip_stack[s_clip_depth][2] = c->clip_x1;
        s_clip_stack[s_clip_depth][3] = c->clip_y1;
        s_clip_depth++;
    }
    int x0 = x > c->clip_x0 ? x : c->clip_x0;
    int y0 = y > c->clip_y0 ? y : c->clip_y0;
    int x1 = (x + w) < c->clip_x1 ? (x + w) : c->clip_x1;
    int y1 = (y + h) < c->clip_y1 ? (y + h) : c->clip_y1;
    c->clip_x0 = (int16_t)x0;
    c->clip_y0 = (int16_t)y0;
    c->clip_x1 = (int16_t)(x1 < x0 ? x0 : x1);
    c->clip_y1 = (int16_t)(y1 < y0 ? y0 : y1);
}

void ui_clip_pop(ui_canvas_t *c)
{
    if (s_clip_depth > 0) {
        s_clip_depth--;
        c->clip_x0 = s_clip_stack[s_clip_depth][0];
        c->clip_y0 = s_clip_stack[s_clip_depth][1];
        c->clip_x1 = s_clip_stack[s_clip_depth][2];
        c->clip_y1 = s_clip_stack[s_clip_depth][3];
    }
}

void ui_clear(ui_canvas_t *c)
{
    memset(c->buf, 0x00, UI_STRIDE * UI_H);
}

void ui_fill(ui_canvas_t *c)
{
    memset(c->buf, 0xFF, UI_STRIDE * UI_H);
}

void ui_px_set(ui_canvas_t *c, int x, int y, bool on)
{
    if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1) {
        return;
    }
    if (c->invert) {
        on = !on;
    }
    uint8_t *p = &c->buf[y * UI_STRIDE + (x >> 3)];
    uint8_t m = (uint8_t)(1u << (x & 7));
    if (on) {
        *p |= m;
    } else {
        *p &= (uint8_t)~m;
    }
}

void ui_px(ui_canvas_t *c, int x, int y)
{
    ui_px_set(c, x, y, true);
}

bool ui_px_get(const ui_canvas_t *c, int x, int y)
{
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) {
        return false;
    }
    return (c->buf[y * UI_STRIDE + (x >> 3)] >> (x & 7)) & 1;
}

void ui_hline(ui_canvas_t *c, int x, int y, int w)
{
    for (int i = 0; i < w; i++) {
        ui_px_set(c, x + i, y, true);
    }
}

void ui_vline(ui_canvas_t *c, int x, int y, int h)
{
    for (int i = 0; i < h; i++) {
        ui_px_set(c, x, y + i, true);
    }
}

void ui_line(ui_canvas_t *c, int x0, int y0, int x1, int y1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        ui_px_set(c, x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ui_rect(ui_canvas_t *c, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    ui_hline(c, x, y, w);
    ui_hline(c, x, y + h - 1, w);
    ui_vline(c, x, y, h);
    ui_vline(c, x + w - 1, y, h);
}

void ui_fill_rect(ui_canvas_t *c, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        ui_hline(c, x, y + j, w);
    }
}

static void corner(ui_canvas_t *c, int cx, int cy, int r, int qx, int qy, bool fill)
{
    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) {
                continue;
            }
            if (!fill && d2 < (r - 1) * (r - 1)) {
                continue;
            }
            ui_px_set(c, cx + qx * dx, cy + qy * dy, true);
        }
    }
}

static void rrect_impl(ui_canvas_t *c, int x, int y, int w, int h, int r, bool fill)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (r * 2 >= w) {
        r = w / 2 - 1;
    }
    if (r * 2 >= h) {
        r = h / 2 - 1;
    }
    if (r <= 0) {
        if (fill) {
            ui_fill_rect(c, x, y, w, h);
        } else {
            ui_rect(c, x, y, w, h);
        }
        return;
    }
    if (fill) {
        ui_fill_rect(c, x + r, y, w - 2 * r, h);
        ui_fill_rect(c, x, y + r, r, h - 2 * r);
        ui_fill_rect(c, x + w - r, y + r, r, h - 2 * r);
    } else {
        ui_hline(c, x + r, y, w - 2 * r);
        ui_hline(c, x + r, y + h - 1, w - 2 * r);
        ui_vline(c, x, y + r, h - 2 * r);
        ui_vline(c, x + w - 1, y + r, h - 2 * r);
    }
    corner(c, x + r, y + r, r, -1, -1, fill);
    corner(c, x + w - 1 - r, y + r, r, 1, -1, fill);
    corner(c, x + r, y + h - 1 - r, r, -1, 1, fill);
    corner(c, x + w - 1 - r, y + h - 1 - r, r, 1, 1, fill);
}

void ui_rrect(ui_canvas_t *c, int x, int y, int w, int h, int r)
{
    rrect_impl(c, x, y, w, h, r, false);
}

void ui_fill_rrect(ui_canvas_t *c, int x, int y, int w, int h, int r)
{
    rrect_impl(c, x, y, w, h, r, true);
}

void ui_circle(ui_canvas_t *c, int cx, int cy, int r)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        ui_px_set(c, cx + x, cy + y, true);
        ui_px_set(c, cx + y, cy + x, true);
        ui_px_set(c, cx - y, cy + x, true);
        ui_px_set(c, cx - x, cy + y, true);
        ui_px_set(c, cx - x, cy - y, true);
        ui_px_set(c, cx - y, cy - x, true);
        ui_px_set(c, cx + y, cy - x, true);
        ui_px_set(c, cx + x, cy - y, true);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void ui_fill_circle(ui_canvas_t *c, int cx, int cy, int r)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) {
            w++;
        }
        ui_hline(c, cx - w, cy + y, 2 * w + 1);
    }
}

void ui_disc_arc(ui_canvas_t *c, int cx, int cy, int r, int start_deg, int end_deg, int thickness)
{
    if (end_deg < start_deg) {
        int t = start_deg;
        start_deg = end_deg;
        end_deg = t;
    }
    for (int rr = r - thickness + 1; rr <= r; rr++) {
        if (rr <= 0) {
            continue;
        }
        for (int a = start_deg; a <= end_deg; a++) {
            float rad = (float)a * 3.14159265f / 180.0f;
            int x = cx + (int)(cosf(rad) * (float)rr + 0.5f);
            int y = cy + (int)(sinf(rad) * (float)rr + 0.5f);
            ui_px_set(c, x, y, true);
        }
    }
}


/* ------------------------------------------------------------------ */
/* UTF-8 解码 + CJK 尺寸选择                                           */
/* ------------------------------------------------------------------ */


/* 单个码位的显示宽度（含 1px 字距） */
static const uint16_t *glyph_ptr(ui_font_t font, unsigned char ch)
{
    if (font >= UI_FONT_MAX) {
        font = UI_FONT_SMALL;
    }
    if (ch < GLYPH_FIRST || ch > GLYPH_LAST) {
        ch = '?';
    }
    return s_fonts[font].data + (size_t)(ch - GLYPH_FIRST) * s_fonts[font].h;
}

static int char_advance(uint32_t cp, ui_font_t font)
{
    ui_font_t af = font;
    uint8_t px = cjk_px_for_font(font);
    if (cp < 0x80) {
        return ui_font_w(af) + 1;
    }
    uint8_t gw = 0, gh = 0, gs = 0;
    if (ui_cjk_glyph(cp, px, &gw, &gh, &gs) != NULL) {
        return (int)gw + 1;
    }
    return (int)px + 1;
}

void ui_text(ui_canvas_t *c, int x, int y, const char *s, ui_font_t font)
{
    if (s == NULL) {
        return;
    }
    int fw = ui_font_w(font);
    int fh = ui_font_h(font);
    int latin_y = y + (font == UI_FONT_MED ? 2 : font == UI_FONT_BIG ? 1 : 0);
    uint8_t cjk_px = cjk_px_for_font(font);
    int cx = x;
    const char *p = s;

    while (*p) {
        uint32_t cp = 0;
        if (utf8_next(&p, &cp) == 0) {
            break;
        }

        if (cp < 0x80) {
            const uint16_t *g = glyph_ptr(font, (unsigned char)cp);
            for (int row = 0; row < fh; row++) {
                uint16_t bits = g[row];
                if (!bits) {
                    continue;
                }
                for (int col = 0; col < fw; col++) {
                    if (bits & (1u << col)) {
                        ui_px_set(c, cx + col, latin_y + row, true);
                    }
                }
            }
            cx += fw + 1;
            continue;
        }

        uint8_t gw = 0, gh = 0, gs = 0;
        const uint8_t *bm = ui_cjk_glyph(cp, cjk_px, &gw, &gh, &gs);
        if (bm == NULL) {
            ui_text(c, cx, y, "?", font);
            cx += (int)cjk_px + 1;
            continue;
        }
        int oy = y + (font == UI_FONT_BIG ? 1 : 0);
        for (int row = 0; row < gh; row++) {
            const uint8_t *line = bm + row * gs;
            for (int col = 0; col < gw; col++) {
                if (line[col >> 3] & (0x80 >> (col & 7))) {
                    ui_px_set(c, cx + col, oy + row, true);
                }
            }
        }
        cx += (int)gw + 1;
    }
}

void ui_text_fg_bg(ui_canvas_t *c, int x, int y, int w, const char *s, ui_font_t font)
{
    int h = ui_text_h(font);
    ui_fill_rect(c, x, y, w, h);
    bool old = c->invert;
    c->invert = true;
    ui_text(c, x, y, s, font);
    c->invert = old;
}

int ui_text_w(const char *s, ui_font_t font)
{
    if (s == NULL) {
        return 0;
    }
    int w = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = 0;
        if (utf8_next(&p, &cp) == 0) {
            break;
        }
        if (cp < 0x80) {
            w += ui_font_w(font) + 1;
        } else {
            w += char_advance(cp, font);
        }
    }
    return w > 0 ? w - 1 : 0;
}

int ui_text_ellipsis(ui_canvas_t *c, int x, int y, int max_w, const char *s, ui_font_t font)
{
    if (s == NULL || max_w <= 0) {
        return 0;
    }
    int total = ui_text_w(s, font);
    if (total <= max_w) {
        ui_text(c, x, y, s, font);
        return total;
    }

    const char *dots = "…";
    if (ui_text_w(dots, font) > max_w) {
        dots = ".";
    }
    if (ui_text_w(dots, font) > max_w) {
        dots = "";
    }
    int dot_w = ui_text_w(dots, font);
    int budget = max_w - dot_w;

    char buf[256];
    size_t o = 0;
    int w = 0;
    const char *p = s;
    while (*p) {
        const char *save = p;
        uint32_t cp = 0;
        if (utf8_next(&p, &cp) == 0) {
            break;
        }
        int adv = char_advance(cp, font);
        if (w + adv > budget) {
            break;
        }
        size_t nbytes = (size_t)(p - save);
        if (o + nbytes >= sizeof(buf) - 1) {
            break;
        }
        memcpy(buf + o, save, nbytes);
        o += nbytes;
        w += adv;
    }
    buf[o] = '\0';

    ui_clip_push_rect(c, x, y, max_w, ui_text_h(font));
    ui_text(c, x, y, buf, font);
    ui_text(c, x + w, y, dots, font);
    ui_clip_pop(c);
    return w + dot_w;
}

/* ------------------------------------------------------------------ */
/* 位图 / 进度条 / 抖动                                                */
/* ------------------------------------------------------------------ */
void ui_bitmap(ui_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (bits[j * stride + (i >> 3)] & (0x80u >> (i & 7))) {
                ui_px_set(c, x + i, y + j, true);
            }
        }
    }
}

void ui_progress(ui_canvas_t *c, int x, int y, int w, int h, uint8_t pct, bool rounded)
{
    if (rounded) {
        ui_rrect(c, x, y, w, h, h / 2);
    } else {
        ui_rect(c, x, y, w, h);
    }
    if (pct > 100) {
        pct = 100;
    }
    int inner_w = (w - 4) * pct / 100;
    if (inner_w <= 0) {
        return;
    }
    if (rounded) {
        ui_fill_rrect(c, x + 2, y + 2, inner_w, h - 4, (h - 4) / 2);
    } else {
        ui_fill_rect(c, x + 2, y + 2, inner_w, h - 4);
    }
}

uint8_t ui_bayer4(int x, int y)
{
    static const uint8_t m[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    return m[y & 3][x & 3];
}

void ui_blend(ui_canvas_t *out, const uint8_t *a, const uint8_t *b, int mode, uint8_t t)
{
    for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) {
            bool use_b = false;
            int sx = x, sy = y;

            switch (mode) {
            case UI_TRANS_SLIDE_L: { /* b 从右侧滑入 */
                int shift = (int)t * UI_W / 255;
                int bx = x - (UI_W - shift);
                if (bx >= 0) {
                    use_b = true;
                    sx = bx;
                }
                break;
            }
            case UI_TRANS_SLIDE_R: { /* b 从左侧滑入 */
                int shift = (int)t * UI_W / 255;
                if (x < shift) {
                    use_b = true;
                    sx = x - shift + UI_W;
                }
                break;
            }
            case UI_TRANS_SLIDE_U: { /* b 从下方滑入 */
                int shift = (int)t * UI_H / 255;
                int by = y - (UI_H - shift);
                if (by >= 0) {
                    use_b = true;
                    sy = by;
                }
                break;
            }
            case UI_TRANS_WIPE_V: {
                use_b = (y < (int)t * UI_H / 255);
                break;
            }
            case UI_TRANS_DISSOLVE:
            default: {
                use_b = ((int)t > (int)(ui_bayer4(x, y) * 16 + 8));
                break;
            }
            }

            const uint8_t *s = use_b ? b : a;
            bool on = (s[sy * UI_STRIDE + (sx >> 3)] >> (sx & 7)) & 1;
            ui_px_set(out, x, y, on);
        }
    }
}

void ui_blit_offset(ui_canvas_t *dst, const uint8_t *src, int dy)
{
    for (int y = 0; y < UI_H; y++) {
        int sy = y - dy;
        for (int x = 0; x < UI_W; x++) {
            bool on = false;
            if (sy >= 0 && sy < UI_H) {
                on = (src[sy * UI_STRIDE + (x >> 3)] >> (x & 7)) & 1;
            }
            ui_px_set(dst, x, y, on);
        }
    }
}
