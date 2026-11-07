#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_gfx.h"
#include "ui_internal.h"
#include "ui_ssd1306.h"
#include "ui_widgets.h"

/* 页面实现依赖的业务模块 */
#include "bt_audio.h"
#include "music.h"
#include "net_service.h"
#include "bsp.h"

static const char *TAG = "ui";

/* ------------------------------------------------------------------ */
/* 双缓冲 + 过渡                                                        */
/* ------------------------------------------------------------------ */
#define FB_SIZE (UI_STRIDE * UI_H)

static uint8_t s_fb_cur[FB_SIZE];
static uint8_t s_fb_prev[FB_SIZE];
static uint8_t s_fb_out[FB_SIZE];

static ui_canvas_t s_cv_cur, s_cv_out;

static const ui_page_t *s_page_cur = &ui_page_boot;
static const ui_page_t *s_page_prev;

static struct {
    bool active;
    int mode;
    uint32_t elapsed_ms;
    uint32_t dur_ms;
} s_trans;

static ui_view_t s_view;
static uint32_t s_ticks_ms;
static bool s_dirty = true;

/* Temporary volume view */
static struct {
    bool visible;
    uint8_t value;
    uint32_t hold_ms;
} s_vol_osd;

/* 息屏 */
static uint32_t s_idle_ms;
static bool s_screen_off;

const ui_view_t *ui_view(void)
{
    return &s_view;
}

uint32_t ui_ticks_ms(void)
{
    return s_ticks_ms;
}

const ui_page_t *ui_current_page(void)
{
    return s_page_cur;
}

void ui_view_refresh(void)
{
    const music_item_t *it = player_get_current();
    s_view.player_state = (int)player_get_state();
    s_view.pos_ms = player_get_pos_ms();
    s_view.dur_ms = player_get_dur_ms();
    s_view.track_total = music_lib_count();
    s_view.track_index = it ? 0 : -1;
    s_view.bt_state = (int)bt_audio_state();
    snprintf(s_view.peer, sizeof(s_view.peer), "%s", bt_audio_connected_name());
    s_view.volume = app_settings_get()->volume;
    s_view.sd_mounted = bsp_sd_is_mounted();
    s_view.lib_count = music_lib_count();
    s_view.ap_running = net_service_ap_running();
    s_view.net_clients = net_service_clients();
}

void ui_view_mark_dirty(void)
{
    s_dirty = true;
}

/* ------------------------------------------------------------------ */
/* 导航                                                                */
/* ------------------------------------------------------------------ */
static const ui_page_t *s_route_pages[5] = {
    &ui_page_now, &ui_page_list, &ui_page_bt, &ui_page_set, &ui_page_about,
};

void ui_goto(ui_route_t route, int mode)
{
    if (route < 0 || route > UI_GO_ABOUT) {
        return;
    }
    ui_navigate(s_route_pages[route], mode);
}

void ui_navigate(const ui_page_t *page, int mode)
{
    if (page == NULL || page == s_page_cur) {
        return;
    }
    s_vol_osd.visible = false;
    /* 旧画面留档，用于过渡动画 */
    memcpy(s_fb_prev, s_fb_cur, FB_SIZE);
    s_page_prev = s_page_cur;
    s_page_cur = page;

    if (s_page_prev && s_page_prev->leave) {
        s_page_prev->leave();
    }
    s_dirty = true;

    /* 先把新页面渲染到当前缓冲 */
    ui_canvas_bind(&s_cv_cur, s_fb_cur);
    ui_clear(&s_cv_cur);
    if (page->enter) {
        page->enter();
    }
    if (page->draw) {
        page->draw(&s_cv_cur);
    }

    s_trans.active = true;
    s_trans.mode = mode % UI_TRANS_COUNT;
    s_trans.elapsed_ms = 0;
    s_trans.dur_ms = 260; /* 过渡时长 */
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */
static void handle_input(app_key_t key, int32_t value)
{
    s_idle_ms = 0;
    if (s_screen_off) {
        s_screen_off = false;
        ssd1306_display_on(true);
        s_dirty = true;
        return;
    }
    if (key == APP_KEY_KEY0_LONG) {
        key = APP_KEY_MENU;
    }
    if (key == APP_KEY_PAGE_PREV || key == APP_KEY_PAGE_NEXT) {
        const int count = sizeof(s_route_pages) / sizeof(s_route_pages[0]);
        const int direction = key == APP_KEY_PAGE_NEXT ? 1 : -1;
        for (int i = 0; i < count; i++) {
            if (s_route_pages[i] == s_page_cur) {
                ui_goto((ui_route_t)((i + direction + count) % count),
                        direction > 0 ? UI_TRANS_SLIDE_L : UI_TRANS_SLIDE_R);
                break;
            }
        }
        s_dirty = true;
        return;
    }
    if (s_page_cur && s_page_cur->input && s_page_cur->input(key, value)) {
        s_dirty = true;
        return;
    }
    /* 默认导航：MENU 顺序切页，BACK 回到正在播放 */
    switch (key) {
    case APP_KEY_MENU: {
        int next = 0;
        for (int i = 0; i < 5; i++) {
            if (s_route_pages[i] == s_page_cur) {
                next = (i + 1) % 5;
                break;
            }
        }
        ui_goto((ui_route_t)next, UI_TRANS_SLIDE_L);
        break;
    }
    case APP_KEY_BACK:
        ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
        break;
    default:
        break;
    }
    s_dirty = true;
}

static void handle_event(const app_evt_t *evt)
{
    switch (evt->id) {
    case APP_EVT_INPUT:
        handle_input((app_key_t)evt->input.key, evt->input.value);
        break;
    case APP_EVT_BT_VOLUME:
        s_view.volume = evt->vol.value;
        s_vol_osd.visible = s_page_cur != &ui_page_set;
        s_vol_osd.value = evt->vol.value;
        s_vol_osd.hold_ms = 1200;
        s_dirty = true;
        break;
    case APP_EVT_BT_STATE:
    case APP_EVT_LIB_READY:
        ui_view_refresh();
        s_dirty = true;
        break;
    case APP_EVT_PLAYER_STATE:
    case APP_EVT_TRACK_CHANGED:
    case APP_EVT_TRACK_POS:
    case APP_EVT_BT_PEER:
    case APP_EVT_BT_DISCOVERY:
    case APP_EVT_SD_STATE:
    case APP_EVT_NET_STATE:
    case APP_EVT_SETTINGS:
    case APP_EVT_AUDIO_LEVEL:
        ui_view_refresh();
        s_dirty = true;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 渲染与刷新                                                          */
/* ------------------------------------------------------------------ */
static uint8_t diff_pages(const uint8_t *a, const uint8_t *b)
{
    uint8_t mask = 0;
    for (int p = 0; p < 8; p++) {
        if (memcmp(a + p * 128, b + p * 128, 128) != 0) {
            mask |= (uint8_t)(1u << p);
        }
    }
    return mask;
}

static void render_frame(uint32_t dt_ms)
{
    if (s_page_cur && s_page_cur->update) {
        s_page_cur->update(dt_ms);
    }

    if (s_trans.active) {
        s_trans.elapsed_ms += dt_ms;
        int t = (int)(s_trans.elapsed_ms * 255 / s_trans.dur_ms);
        if (t > 255) {
            t = 255;
        }
        ui_canvas_bind(&s_cv_out, s_fb_out);
        ui_clip_reset(&s_cv_out);
        ui_blend(&s_cv_out, s_fb_prev, s_fb_cur, s_trans.mode, (uint8_t)t);
        ssd1306_flush(s_fb_out, 0xFF);
        if (t >= 255) {
            s_trans.active = false;
            memcpy(s_fb_prev, s_fb_cur, FB_SIZE);
        }
        return;
    }

    /* 静态/局部刷新：整页重绘到 cur，再按 page 差分 */
    ui_canvas_bind(&s_cv_cur, s_fb_cur);
    ui_clear(&s_cv_cur);
    if (s_page_cur && s_page_cur->draw) {
        s_page_cur->draw(&s_cv_cur);
    }
    if (s_vol_osd.visible) {
        uiw_volume_view(&s_cv_cur, s_vol_osd.value);
    }

    if (s_dirty) {
        ssd1306_flush(s_fb_cur, 0xFF);
        memcpy(s_fb_prev, s_fb_cur, FB_SIZE);
        s_dirty = false;
        return;
    }

    uint8_t mask = diff_pages(s_fb_cur, s_fb_prev);
    if (mask) {
        ssd1306_flush(s_fb_cur, mask);
        memcpy(s_fb_prev, s_fb_cur, FB_SIZE);
    }
}

/* ------------------------------------------------------------------ */
/* 任务                                                                */
/* ------------------------------------------------------------------ */
static void ui_task(void *arg)
{
    (void)arg;
    uint32_t last_us = (uint32_t)esp_timer_get_time();
    const TickType_t period = pdMS_TO_TICKS(33);

    for (;;) {
        uint32_t now_us = (uint32_t)esp_timer_get_time();
        uint32_t dt_ms = (now_us - last_us) / 1000;
        last_us = now_us;
        s_ticks_ms += dt_ms;

        app_evt_t evt;
        while (app_evt_wait(&evt, 0)) {
            handle_event(&evt);
        }

        if (s_vol_osd.visible) {
            if (s_vol_osd.hold_ms > dt_ms) {
                s_vol_osd.hold_ms -= dt_ms;
            } else {
                s_vol_osd.visible = false;
                s_dirty = true;
            }
        }

        /* 息屏 */
        const app_settings_t *cfg = app_settings_get();
        if (cfg->screen_timeout_s > 0 && !s_screen_off) {
            s_idle_ms += dt_ms;
            if (s_idle_ms > (uint32_t)cfg->screen_timeout_s * 1000) {
                s_screen_off = true;
                ssd1306_display_on(false);
            }
        } else {
            s_idle_ms = 0;
        }

        if (ssd1306_present() && !s_screen_off) {
            render_frame(dt_ms);
        }
        vTaskDelay(period);
    }
}

esp_err_t ui_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    memset(&s_trans, 0, sizeof(s_trans));

    esp_err_t err = ssd1306_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel not available, UI runs headless");
    } else {
        ssd1306_set_contrast(0x20 + app_settings_get()->brightness * 0x28);
    }

    ui_canvas_bind(&s_cv_cur, s_fb_cur);
    ui_clear(&s_cv_cur);
    ui_canvas_bind(&s_cv_out, s_fb_out);
    ui_clear(&s_cv_out);
    memset(s_fb_prev, 0, FB_SIZE);

    s_page_cur = &ui_page_now;  /* 不再有开机动画，直接进主界面 */
    if (s_page_cur->enter) {
        s_page_cur->enter();
    }
    ui_view_refresh();

    BaseType_t ok = xTaskCreate(ui_task, "ui", 16384, NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool ui_has_panel(void)
{
    return ssd1306_present();
}
