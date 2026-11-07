#include "ui_widgets.h"

#include <stdio.h>

#include "ui_theme.h"

void uiw_marquee_text(ui_canvas_t *c, int x, int y, int max_w, const char *text, ui_font_t font,
                      uint32_t elapsed_ms)
{
    if (text == NULL || max_w <= 0) {
        return;
    }
    int width = ui_text_w(text, font);
    if (width <= max_w) {
        ui_text(c, x, y, text, font);
        return;
    }

    const uint32_t hold_ms = 800;
    const uint32_t travel_ms = (uint32_t)(width - max_w) * 45;
    const uint32_t cycle_ms = 2 * (hold_ms + travel_ms);
    uint32_t phase = elapsed_ms % cycle_ms;
    int shift;
    if (phase < hold_ms) {
        shift = 0;
    } else if (phase < hold_ms + travel_ms) {
        shift = (int)((phase - hold_ms) / 45);
    } else if (phase < 2 * hold_ms + travel_ms) {
        shift = width - max_w;
    } else {
        shift = width - max_w - (int)((phase - 2 * hold_ms - travel_ms) / 45);
    }
    ui_clip_push_rect(c, x, y, max_w, ui_text_h(font));
    ui_text(c, x - shift, y, text, font);
    ui_clip_pop(c);
}

void uiw_pill_text_scroll(ui_canvas_t *c, int x, int y, int w, int h, int r, const char *text,
                           ui_font_t font, uint32_t elapsed_ms)
{
    ui_fill_rrect(c, x, y, w, h, r);

    bool old = c->invert;
    c->invert = true;
    int dy = (h - ui_text_h(font)) / 2;
    int pad = r + 5;
    int text_x = x + pad;
    int text_w = w - 2 * pad;
    int width = ui_text_w(text, font);
    if (width <= text_w) {
        ui_text(c, text_x, y + dy, text, font);
    } else if (text_w > 0) {
        const int gap = 16;
        const uint32_t hold_ms = 800;
        const uint32_t phase = elapsed_ms % (hold_ms + (uint32_t)(width + gap) * 45);
        int shift = phase < hold_ms ? 0 : (int)((phase - hold_ms) / 45);
        ui_clip_push_rect(c, text_x, y + dy, text_w, ui_text_h(font));
        if (shift < width) {
            ui_text(c, text_x - shift, y + dy, text, font);
        }
        if (width + gap - shift < text_w) {
            ui_text(c, text_x + width + gap - shift, y + dy, text, font);
        }
        ui_clip_pop(c);
    }
    c->invert = old;
}

void uiw_pill_text(ui_canvas_t *c, int x, int y, int w, int h, int r, const char *text, ui_font_t font)
{
    ui_fill_rrect(c, x, y, w, h, r);
    bool old = c->invert;
    c->invert = true;
    int pad = r + 5;
    int dy = (h - ui_text_h(font)) / 2;
    ui_text_ellipsis(c, x + pad, y + dy, w - 2 * pad, text, font);
    c->invert = old;
}

void uiw_menu_row_scroll(ui_canvas_t *c, int x, int y, int w, int h, const char *label,
                         const char *value, bool sel, ui_font_t font, uint32_t elapsed_ms)
{
    if (sel) {
        ui_fill_rrect(c, x, y, w, h, 2);
    }
    bool old = c->invert;
    c->invert = sel;

    int dy = (h - ui_text_h(font)) / 2;
    int value_w = (value && value[0]) ? ui_text_w(value, font) : 0;
    int right_w = value_w > w / 2 ? w / 2 : value_w;
    int label_w = w - 2 * TH_MENU_PADX - (right_w ? right_w + 4 : 0);
    ui_clip_push_rect(c, x + TH_MENU_PADX, y, label_w, h);
    if (sel) {
        uiw_marquee_text(c, x + TH_MENU_PADX, y + dy, label_w, label, font, elapsed_ms);
    } else {
        ui_text_ellipsis(c, x + TH_MENU_PADX, y + dy, label_w, label, font);
    }
    ui_clip_pop(c);

    if (right_w) {
        int vx = x + w - TH_MENU_PADX - right_w;
        ui_text_ellipsis(c, vx, y + dy, right_w, value, font);
    }
    c->invert = old;
}

void uiw_menu_row(ui_canvas_t *c, int x, int y, int w, int h, const char *label, const char *value, bool sel,
                  ui_font_t font)
{
    uiw_menu_row_scroll(c, x, y, w, h, label, value, sel, font, 0);
}

void uiw_kv_row(ui_canvas_t *c, int x, int y, int w, const char *key, const char *value, ui_font_t font)
{
    int value_w = (value && value[0]) ? ui_text_w(value, font) : 0;
    int right_w = value_w > w / 2 ? w / 2 : value_w;
    int key_w = w - (right_w ? right_w + 4 : 0);
    ui_text_ellipsis(c, x, y, key_w, key, font);
    if (right_w) {
        ui_text_ellipsis(c, x + w - right_w, y, right_w, value, font);
    }
}

void uiw_progress(ui_canvas_t *c, int x, int y, int w, int h, int r, uint8_t pct)
{
    (void)r;
    if (pct > 100) {
        pct = 100;
    }
    ui_hline(c, x, y + h / 2, w);
    int fill = w * pct / 100;
    if (fill > 0) {
        ui_fill_rect(c, x, y, fill, h);
    }
}

void uiw_scrollbar(ui_canvas_t *c, int x, int y, int h, int total, int visible, int top)
{
    if (total <= visible || visible <= 0) {
        return;
    }
    int thumb = h * visible / total;
    if (thumb < 8) {
        thumb = 8;
    }
    int max_top = total - visible;
    if (max_top < 1) {
        max_top = 1;
    }
    if (top > max_top) {
        top = max_top;
    }
    int off = (h - thumb) * top / max_top;
    for (int i = 0; i < h; i += 4) {
        ui_px(c, x, y + i);
    }
    ui_vline(c, x, y + off, thumb);
}

void uiw_volume_view(ui_canvas_t *c, uint8_t value)
{
    const int margin = TH_PAGE_MARGIN;
    const int track_y = 37;
    const int track_h = 9;
    const int track_w = UI_W - 2 * margin;
    const int inner_w = track_w - 4;
    int pct = ((int)value * 100 + 63) / 127;
    if (pct > 100) {
        pct = 100;
    }
    char text[8];
    snprintf(text, sizeof(text), "%d%%", pct);

    ui_clear(c);
    ui_clip_reset(c);
    c->invert = false;

    int value_x = UI_W - margin - ui_text_w(text, UI_FONT_BIG);
    ui_text_ellipsis(c, margin, 9, value_x - margin - 4, "音量", UI_FONT_MED);
    ui_text(c, value_x, 4, text, UI_FONT_BIG);
    ui_hline(c, margin, 29, track_w);

    ui_rect(c, margin, track_y, track_w, track_h);
    int fill_w = inner_w * pct / 100;
    if (fill_w > 0) {
        ui_fill_rect(c, margin + 2, track_y + 2, fill_w, track_h - 4);
    }
    ui_text(c, margin, 51, "0", UI_FONT_SMALL);
    ui_text(c, UI_W - margin - ui_text_w("100", UI_FONT_SMALL), 51, "100", UI_FONT_SMALL);
}
