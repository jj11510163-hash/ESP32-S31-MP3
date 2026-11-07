/*
 * ui_internal.h —— UI 内部契约（页面与调度器之间）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_core.h"
#include "ui_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    void (*enter)(void);
    void (*leave)(void);
    void (*update)(uint32_t dt_ms);
    void (*draw)(ui_canvas_t *c);
    bool (*input)(app_key_t key, int32_t value);
} ui_page_t;

extern const ui_page_t ui_page_boot;
extern const ui_page_t ui_page_now;
extern const ui_page_t ui_page_list;
extern const ui_page_t ui_page_bt;
extern const ui_page_t ui_page_set;
extern const ui_page_t ui_page_about;

/* 页面切换（带过渡动画） */
void ui_navigate(const ui_page_t *page, int mode);
const ui_page_t *ui_current_page(void);
uint32_t ui_ticks_ms(void);

/* 供页面之间跳转的一组便捷入口 */
typedef enum {
    UI_GO_NOW = 0,
    UI_GO_LIST,
    UI_GO_BT,
    UI_GO_SETTINGS,
    UI_GO_ABOUT,
} ui_route_t;

void ui_goto(ui_route_t route, int mode);

/* 全局共享的轻量视图状态（由事件更新，页面只读） */
typedef struct {
    int player_state;
    int track_index;
    int track_total;
    uint32_t pos_ms;
    uint32_t dur_ms;
    int bt_state;
    char peer[32];
    uint8_t volume;
    bool sd_mounted;
    int lib_count;
    bool ap_running;
    uint8_t net_clients;
} ui_view_t;

const ui_view_t *ui_view(void);
void ui_view_refresh(void);  /* 从各模块拉一次最新状态 */

#ifdef __cplusplus
}
#endif
