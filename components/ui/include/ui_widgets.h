/*
 * ui_widgets.h —— 可复用的 UI 组件
 *
 * 所有页面都只通过这里画"行、胶囊、进度条"，不直接摆坐标。
 * 这样以后加页面时排版自动一致，不会每个页面各写一套数字。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reverse header with text clipped inside its content area. */
void uiw_pill_text(ui_canvas_t *c, int x, int y, int w, int h, int r, const char *text, ui_font_t font);
void uiw_pill_text_scroll(ui_canvas_t *c, int x, int y, int w, int h, int r, const char *text,
                           ui_font_t font, uint32_t elapsed_ms);

void uiw_marquee_text(ui_canvas_t *c, int x, int y, int max_w, const char *text, ui_font_t font,
                      uint32_t elapsed_ms);

/* 菜单行：左标签 + 右数值；sel=true 时整行反色胶囊 */
void uiw_menu_row(ui_canvas_t *c, int x, int y, int w, int h, const char *label, const char *value, bool sel,
                  ui_font_t font);
void uiw_menu_row_scroll(ui_canvas_t *c, int x, int y, int w, int h, const char *label,
                         const char *value, bool sel, ui_font_t font, uint32_t elapsed_ms);

/* 普通信息行：左标签 + 右数值，不改色 */
void uiw_kv_row(ui_canvas_t *c, int x, int y, int w, const char *key, const char *value, ui_font_t font);

/* 进度条：未播放段用 4:1 抖动做"浅灰"，已播放段实心圆角，末端浮动圆点 */
void uiw_progress(ui_canvas_t *c, int x, int y, int w, int h, int r, uint8_t pct);

/* 右侧滚动指示条 */
void uiw_scrollbar(ui_canvas_t *c, int x, int y, int h, int total, int visible, int top);

/* Full-screen temporary volume view; value uses the AVRCP 0..127 scale. */
void uiw_volume_view(ui_canvas_t *c, uint8_t value);

#ifdef __cplusplus
}
#endif
