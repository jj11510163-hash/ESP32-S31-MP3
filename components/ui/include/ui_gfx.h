/*
 * ui_gfx.h —— 1bpp 画布与绘图原语（128x64 单色 OLED）
 *
 * 画布格式：行优先，每行 stride=(UI_W+7)/8 字节，bit0 = 该字节最左边像素。
 * 所有函数自动裁剪到当前 clip 矩形；invert=true 时按“反色”绘制（用于选中高亮）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_W 128
#define UI_H 64
#define UI_STRIDE (UI_W / 8)

typedef enum {
    UI_FONT_SMALL = 0, /* 6x10  */
    UI_FONT_MED,       /* 7x12 Latin / 12x12 CJK in a 14px line */
    UI_FONT_BIG,       /* 12x20 */
    UI_FONT_MAX
} ui_font_t;

typedef struct {
    uint8_t *buf;
    int16_t clip_x0, clip_y0, clip_x1, clip_y1; /* 含头不含尾 */
    bool invert;
} ui_canvas_t;

void ui_canvas_bind(ui_canvas_t *c, uint8_t *buf);
void ui_clip_reset(ui_canvas_t *c);
void ui_clip_set(ui_canvas_t *c, int x, int y, int w, int h);
void ui_clip_push_rect(ui_canvas_t *c, int x, int y, int w, int h);
void ui_clip_pop(ui_canvas_t *c);

void ui_clear(ui_canvas_t *c);   /* 全黑 */
void ui_fill(ui_canvas_t *c);    /* 全亮 */
void ui_px(ui_canvas_t *c, int x, int y);
void ui_px_set(ui_canvas_t *c, int x, int y, bool on);
bool ui_px_get(const ui_canvas_t *c, int x, int y);

void ui_hline(ui_canvas_t *c, int x, int y, int w);
void ui_vline(ui_canvas_t *c, int x, int y, int h);
void ui_line(ui_canvas_t *c, int x0, int y0, int x1, int y1);
void ui_rect(ui_canvas_t *c, int x, int y, int w, int h);
void ui_fill_rect(ui_canvas_t *c, int x, int y, int w, int h);
void ui_rrect(ui_canvas_t *c, int x, int y, int w, int h, int r);
void ui_fill_rrect(ui_canvas_t *c, int x, int y, int w, int h, int r);
void ui_circle(ui_canvas_t *c, int cx, int cy, int r);
void ui_fill_circle(ui_canvas_t *c, int cx, int cy, int r);
void ui_disc_arc(ui_canvas_t *c, int cx, int cy, int r, int start_deg, int end_deg, int thickness);

/* 文本：y 为文字顶部；bg=true 时先在下方铺满实心块再反色写字（选中态） */
void ui_text(ui_canvas_t *c, int x, int y, const char *s, ui_font_t font);
void ui_text_fg_bg(ui_canvas_t *c, int x, int y, int w, const char *s, ui_font_t font);
int ui_text_w(const char *s, ui_font_t font);
int ui_font_h(ui_font_t font);
int ui_font_w(ui_font_t font);
int ui_text_h(ui_font_t font); /* line box height, including baseline/descenders */
/* 超宽自动截断加省略号；返回实际绘制宽度 */
int ui_text_ellipsis(ui_canvas_t *c, int x, int y, int max_w, const char *s, ui_font_t font);

/* 位图（1bpp，行优先，MSB 在左）；w 可被 8 整除或任意 */
void ui_bitmap(ui_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits);

/* 进度条：x/y/w/h 外框，pct 0..100 */
void ui_progress(ui_canvas_t *c, int x, int y, int w, int h, uint8_t pct, bool rounded);

/* 4x4 Bayer 抖动阈值（0..15），用于灰阶过渡动画 */
uint8_t ui_bayer4(int x, int y);

/* 两幅画面混合：t=0 全 a，t=255 全 b；mode 见 UI_TRANS_* */
void ui_blend(ui_canvas_t *out, const uint8_t *a, const uint8_t *b, int mode, uint8_t t);

#define UI_TRANS_SLIDE_L 0
#define UI_TRANS_SLIDE_R 1
#define UI_TRANS_SLIDE_U 2
#define UI_TRANS_WIPE_V 3
#define UI_TRANS_DISSOLVE 4
#define UI_TRANS_COUNT 5

/* 把 src 整体上下平移 dy 像素画到 dst（用于滚动动画） */
void ui_blit_offset(ui_canvas_t *dst, const uint8_t *src, int dy);

#ifdef __cplusplus
}
#endif
