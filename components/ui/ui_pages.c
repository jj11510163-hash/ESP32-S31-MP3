/* OLED pages. Navigation and player operations stay outside drawing code. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_core.h"
#include "bt_audio.h"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "music.h"
#include "net_service.h"
#include "ui_gfx.h"
#include "ui_internal.h"
#include "ui_ssd1306.h"
#include "ui_theme.h"
#include "ui_widgets.h"

/* 毫秒 -> "m:ss" */
static void fmt_time(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* ------------------------------------------------------------------ */
/* 1. 开机动画                                                         */
/* ------------------------------------------------------------------ */
static uint32_t s_boot_ms;

static void boot_enter(void)
{
    s_boot_ms = 0;
}

static void boot_update(uint32_t dt)
{
    s_boot_ms += dt;
    const ui_view_t *v = ui_view();
    bool ready = v->sd_mounted || s_boot_ms > 2000;
    if ((ready && s_boot_ms > 1500) || s_boot_ms > 8000) {
        ui_goto(UI_GO_NOW, UI_TRANS_DISSOLVE);
    }
}

static void boot_draw(ui_canvas_t *c)
{
    int prog = (int)(s_boot_ms * 100 / 1500);
    if (prog > 100) {
        prog = 100;
    }
    const char *title = "S31 PLAYER";
    int tw = ui_text_w(title, UI_FONT_MED);
    ui_text(c, (UI_W - tw) / 2, 10, title, UI_FONT_MED);

    for (int x = TH_PAGE_MARGIN; x < TH_W - TH_PAGE_MARGIN; x++) {
        if ((x - TH_PAGE_MARGIN) * 100 / TH_CONTENT_W > prog) {
            break;
        }
        float phase = (float)x * 0.13f + (float)s_boot_ms * 0.004f;
        int y = 35 + (int)(sinf(phase) * 4.0f);
        ui_px(c, x, y);
    }

    const char *status = "正在启动";
    ui_text(c, (UI_W - ui_text_w(status, UI_FONT_MED)) / 2, 44, status, UI_FONT_MED);
    uiw_progress(c, TH_PAGE_MARGIN, 60, TH_CONTENT_W, 2, 0, (uint8_t)prog);
}

const ui_page_t ui_page_boot = {
    .name = "boot",
    .enter = boot_enter,
    .update = boot_update,
    .draw = boot_draw,
    .input = NULL,
};

/* ------------------------------------------------------------------ */
/* 2. 正在播放                                                         */
/* ------------------------------------------------------------------ */
static uint32_t s_now_ms;

static void now_enter(void)
{
    s_now_ms = 0;
}

static void now_update(uint32_t dt)
{
    s_now_ms += dt;
}

static void now_state_icon(ui_canvas_t *c, player_state_t state)
{
    if (state == PLAYER_PLAYING) {
        ui_fill_rect(c, 9, 42, 2, 7);
        ui_fill_rect(c, 13, 42, 2, 7);
    } else if (state == PLAYER_PAUSED || state == PLAYER_LOADING) {
        for (int row = 0; row < 7; row++) {
            int half = row < 4 ? row : 6 - row;
            ui_hline(c, 9, 42 + row, half + 1);
        }
    } else {
        ui_rect(c, 9, 43, 6, 6);
    }
}

static void now_draw(ui_canvas_t *c)
{
    const music_item_t *it = player_get_current();
    const bool has = (it != NULL);

    uiw_pill_text_scroll(c, TH_PAGE_MARGIN, TH_NP_TITLE_Y, TH_CONTENT_W, TH_NP_TITLE_H, 2,
                          (has && it->title[0]) ? it->title : "无曲目", UI_FONT_MED, s_now_ms);

    ui_text_ellipsis(c, TH_PAGE_MARGIN, TH_NP_ART_Y, TH_CONTENT_W,
                     (has && it->artist[0]) ? it->artist
                                            : (music_lib_scanning() ? "正在扫描" : "未知艺术家"),
                     UI_FONT_MED);

    uint32_t dur = player_get_dur_ms();
    uint32_t pos = player_get_pos_ms();
    uint8_t pct = (dur > 0) ? (uint8_t)((uint64_t)pos * 100 / dur) : 0;

    now_state_icon(c, player_get_state());
    uiw_progress(c, 23, TH_NP_BAR_Y, 97, TH_NP_BAR_H, 0, pct);

    char t1[12], t2[12];
    fmt_time(t1, sizeof(t1), pos);
    if (dur > 0) {
        fmt_time(t2, sizeof(t2), dur);
    } else {
        snprintf(t2, sizeof(t2), "--:--");
    }
    ui_text(c, TH_PAGE_MARGIN, TH_NP_TIME_Y, t1, UI_FONT_MED);
    ui_text(c, TH_W - TH_PAGE_MARGIN - ui_text_w(t2, UI_FONT_MED), TH_NP_TIME_Y, t2, UI_FONT_MED);
}

static bool now_input(app_key_t key, int32_t value)
{
    (void)value;
    switch (key) {
    case APP_KEY_PLAY:
    case APP_KEY_KEY0_SINGLE:
        player_toggle();
        return true;
    case APP_KEY_NEXT:
    case APP_KEY_KEY0_DOUBLE:
        player_next();
        return true;
    case APP_KEY_PREV:
    case APP_KEY_KEY0_TRIPLE:
        player_prev();
        return true;
    case APP_KEY_ENTER:
        ui_goto(UI_GO_LIST, UI_TRANS_SLIDE_L);
        return true;
    case APP_KEY_ROT_CW:
    case APP_KEY_UP:
        app_settings_volume_step(+1, 4);
        bt_audio_set_absolute_volume(app_settings_get()->volume);
        return true;
    case APP_KEY_ROT_CCW:
    case APP_KEY_DOWN:
        app_settings_volume_step(-1, 4);
        bt_audio_set_absolute_volume(app_settings_get()->volume);
        return true;
    default:
        return false;
    }
}

const ui_page_t ui_page_now = {
    .name = "now",
    .enter = now_enter,
    .update = now_update,
    .draw = now_draw,
    .input = now_input,
};

/* ------------------------------------------------------------------ */
/* 3. 曲库列表                                                         */
/* ------------------------------------------------------------------ */
static int s_sel;
static int s_top;
static uint32_t s_list_ms;

static void list_enter(void)
{
    s_list_ms = 0;
    const music_item_t *cur = player_get_current();
    int idx = 0;
    int n = music_lib_count();
    for (int i = 0; i < n; i++) {
        const music_item_t *it = music_lib_get(i);
        if (it == cur) {
            idx = i;
            break;
        }
    }
    s_sel = idx;
    s_top = (s_sel / TH_MENU_ROWS) * TH_MENU_ROWS;
}

static void list_update(uint32_t dt)
{
    s_list_ms += dt;
}

static void list_draw(ui_canvas_t *c)
{
    int n = music_lib_count();
    if (n == 0) {
        ui_text(c, TH_PAGE_MARGIN, 17, "曲库为空", UI_FONT_MED);
        ui_text(c, TH_PAGE_MARGIN, 37, "请放入 /music", UI_FONT_MED);
        return;
    }
    if (s_sel >= n) {
        s_sel = n - 1;
    }

    if (s_sel < s_top) {
        s_top = s_sel;
    } else if (s_sel >= s_top + TH_MENU_ROWS) {
        s_top = s_sel - TH_MENU_ROWS + 1;
    }

    for (int r = 0; r < TH_MENU_ROWS; r++) {
        int i = s_top + r;
        if (i >= n) {
            break;
        }
        const music_item_t *it = music_lib_get(i);
        int y = TH_MENU_TOP + r * TH_ROW_H;
        bool sel = (i == s_sel);

        char dur[10];
        if (it->duration_ms) {
            fmt_time(dur, sizeof(dur), it->duration_ms);
        } else {
            snprintf(dur, sizeof(dur), "--:--");
        }
        uiw_menu_row_scroll(c, TH_MENU_X, y, TH_MENU_W, TH_ROW_H,
                             it->title[0] ? it->title : it->path, dur, sel, UI_FONT_MED, s_list_ms);
    }

    uiw_scrollbar(c, TH_SCROLLBAR_X, TH_MENU_TOP, TH_ROW_H * TH_MENU_ROWS, n, TH_MENU_ROWS, s_top);
}

static bool list_input(app_key_t key, int32_t value)
{
    (void)value;
    int n = music_lib_count();
    if (n == 0) {
        return false;
    }
    if (s_sel >= n) {
        s_sel = n - 1;
    }
    int old = s_sel;
    switch (key) {
    case APP_KEY_DOWN:
    case APP_KEY_ROT_CW:
    case APP_KEY_KEY0_SINGLE:
        s_sel = (s_sel + 1) % n;
        break;
    case APP_KEY_UP:
    case APP_KEY_ROT_CCW:
    case APP_KEY_KEY0_TRIPLE:
        s_sel = (s_sel - 1 + n) % n;
        break;
    case APP_KEY_ENTER:
    case APP_KEY_PLAY:
    case APP_KEY_KEY0_DOUBLE:
        player_start_index(s_sel, true);
        if (key == APP_KEY_KEY0_DOUBLE) {
            ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
        }
        return true;
    case APP_KEY_PREV:
        player_prev();
        return true;
    case APP_KEY_NEXT:
        player_next();
        return true;
    case APP_KEY_BACK:
        ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
        return true;
    default:
        return false;
    }
    if (s_sel < s_top) {
        s_top = s_sel;
    } else if (s_sel >= s_top + TH_MENU_ROWS) {
        s_top = s_sel - TH_MENU_ROWS + 1;
    }
    if (s_sel != old) {
        s_list_ms = 0;
    }
    return s_sel != old;
}

const ui_page_t ui_page_list = {
    .name = "list",
    .enter = list_enter,
    .update = list_update,
    .draw = list_draw,
    .input = list_input,
};

/* ------------------------------------------------------------------ */
/* 4. 蓝牙                                                             */
/* ------------------------------------------------------------------ */
static int s_bt_sel;
static int s_bt_top;
static int s_bt_known_count;
static uint32_t s_bt_ms;
static bool s_bt_scan_pending;
static uint32_t s_bt_fail_ms;
static bt_state_t s_bt_prev_state;

static void bt_enter(void)
{
    s_bt_ms = 0;
    s_bt_sel = 0;
    s_bt_top = 0;
    s_bt_known_count = bt_audio_peer_count();
    s_bt_fail_ms = 0;
    s_bt_prev_state = bt_audio_state();
    s_bt_scan_pending = bt_audio_peer_count() == 0 &&
                        s_bt_prev_state != BT_ST_CONNECTED &&
                        s_bt_prev_state != BT_ST_STREAMING &&
                        s_bt_prev_state != BT_ST_CONNECTING &&
                        s_bt_prev_state != BT_ST_DISCOVERING;
}

static void bt_leave(void)
{
    s_bt_scan_pending = false;
    bt_audio_stop_discovery();
}

static void bt_update(uint32_t dt)
{
    s_bt_ms += dt;
    bt_state_t st = bt_audio_state();
    if (s_bt_prev_state == BT_ST_CONNECTING &&
        (st == BT_ST_CONNECTED || st == BT_ST_STREAMING)) {
        s_bt_sel = 0;
        s_bt_top = 0;
    }
    int count = bt_audio_peer_count();
    if (count > s_bt_known_count && s_bt_known_count > 0 &&
        s_bt_sel == s_bt_known_count) {
        s_bt_sel = count;
    }
    s_bt_known_count = count;
    if (s_bt_prev_state == BT_ST_CONNECTING && st == BT_ST_READY && !s_bt_scan_pending) {
        s_bt_fail_ms = 2200;
    }
    if (st == BT_ST_CONNECTED || st == BT_ST_STREAMING) {
        s_bt_fail_ms = 0;
    }
    s_bt_prev_state = st;
    if (s_bt_fail_ms > dt) {
        s_bt_fail_ms -= dt;
    } else {
        s_bt_fail_ms = 0;
    }
    if (s_bt_scan_pending && st == BT_ST_READY && !bt_audio_discovering()) {
        s_bt_scan_pending = false;
        if (bt_audio_start_discovery(0) != ESP_OK) {
            s_bt_fail_ms = 2200;
        }
    }
}

static const char *bt_state_cn(bt_state_t st)
{
    switch (st) {
    case BT_ST_OFF:
        return "蓝牙关闭";
    case BT_ST_INITING:
        return "初始化";
    case BT_ST_READY:
        return "待连接";
    case BT_ST_DISCOVERING:
        return "搜索中";
    case BT_ST_CONNECTING:
        return "连接中";
    case BT_ST_CONNECTED:
        return "已连接";
    case BT_ST_STREAMING:
        return "播放中";
    default:
        return "出错了";
    }
}

static void bt_draw(ui_canvas_t *c)
{
    bt_state_t st = bt_audio_state();
    uiw_pill_text(c, TH_PAGE_MARGIN, TH_BT_HEADER_Y, TH_CONTENT_W, TH_BT_HEADER_H, 2,
                  s_bt_fail_ms ? "连接失败" : bt_state_cn(st), UI_FONT_MED);

    int n = bt_audio_peer_count();
    if (st == BT_ST_CONNECTED || st == BT_ST_STREAMING) {
        const char *peer = bt_audio_connected_name();
        uiw_marquee_text(c, TH_PAGE_MARGIN, 19, TH_CONTENT_W,
                         peer && peer[0] ? peer : "已连接", UI_FONT_MED, s_bt_ms);
        if (s_bt_sel > 1) {
            s_bt_sel = 0;
        }
        uiw_menu_row(c, TH_MENU_X, 34, TH_MENU_W, TH_ROW_H, "播放音乐", NULL,
                     s_bt_sel == 0, UI_FONT_MED);
        uiw_menu_row(c, TH_MENU_X, 49, TH_MENU_W, TH_ROW_H, "断开并搜索", NULL,
                     s_bt_sel == 1, UI_FONT_MED);
        return;
    }

    if (st == BT_ST_CONNECTING) {
        uiw_marquee_text(c, TH_PAGE_MARGIN, 24, TH_CONTENT_W,
                         bt_audio_connected_name(), UI_FONT_MED, s_bt_ms);
        return;
    }
    if (n == 0) {
        const char *prompt = st == BT_ST_DISCOVERING ? "正在搜索耳机" : "未找到设备";
        ui_text(c, (TH_W - ui_text_w(prompt, UI_FONT_MED)) / 2, 25, prompt, UI_FONT_MED);
        ui_text(c, (TH_W - ui_text_w("双击重新搜索", UI_FONT_MED)) / 2, 47,
                "双击重新搜索", UI_FONT_MED);
        return;
    }

    int total = n + 1;
    if (s_bt_sel >= total) {
        s_bt_sel = total - 1;
    }
    if (s_bt_sel < s_bt_top) {
        s_bt_top = s_bt_sel;
    } else if (s_bt_sel >= s_bt_top + TH_BT_ROWS) {
        s_bt_top = s_bt_sel - TH_BT_ROWS + 1;
    }
    for (int r = 0; r < TH_BT_ROWS; r++) {
        int i = s_bt_top + r;
        if (i >= total) {
            break;
        }
        int y = TH_BT_ROW_Y + r * TH_ROW_H;
        const char *name = i == n ? "重新搜索" : bt_audio_peer_name(i);
        uiw_menu_row_scroll(c, TH_MENU_X, y, TH_MENU_W, TH_ROW_H, name,
                             NULL, i == s_bt_sel, UI_FONT_MED, s_bt_ms);
    }
    uiw_scrollbar(c, TH_SCROLLBAR_X, TH_BT_ROW_Y, TH_BT_ROWS * TH_ROW_H,
                  total, TH_BT_ROWS, s_bt_top);
}

static bool bt_input(app_key_t key, int32_t value)
{
    (void)value;
    bt_state_t st = bt_audio_state();
    int n = bt_audio_peer_count();
    bool linked = st == BT_ST_CONNECTED || st == BT_ST_STREAMING;
    int total = linked ? 2 : n + 1;
    if (s_bt_sel >= total) {
        s_bt_sel = total - 1;
    }
    switch (key) {
    case APP_KEY_ENTER:
    case APP_KEY_PLAY:
    case APP_KEY_KEY0_DOUBLE:
        if (linked) {
            if (s_bt_sel == 1) {
                s_bt_scan_pending = true;
                s_bt_sel = 0;
                s_bt_top = 0;
                if (bt_audio_disconnect() != ESP_OK) {
                    s_bt_scan_pending = false;
                    s_bt_fail_ms = 2200;
                }
            } else {
                if (player_get_state() == PLAYER_PAUSED) {
                    player_resume();
                } else if (player_get_state() == PLAYER_STOPPED && music_lib_count() > 0) {
                    player_toggle();
                }
                ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
            }
            return true;
        }
        if (st == BT_ST_CONNECTING) {
            return true;
        }
        if (n == 0 || s_bt_sel == n) {
            s_bt_sel = 0;
            s_bt_top = 0;
            s_bt_scan_pending = true;
            bt_audio_stop_discovery();
        } else {
            if (bt_audio_connect(s_bt_sel) != ESP_OK) {
                s_bt_fail_ms = 2200;
            }
        }
        return true;
    case APP_KEY_NEXT:
    case APP_KEY_ROT_CW:
    case APP_KEY_KEY0_SINGLE:
        s_bt_sel = (s_bt_sel + 1) % total;
        s_bt_ms = 0;
        return true;
    case APP_KEY_PREV:
    case APP_KEY_ROT_CCW:
    case APP_KEY_KEY0_TRIPLE:
        s_bt_sel = (s_bt_sel - 1 + total) % total;
        s_bt_ms = 0;
        return true;
    case APP_KEY_BACK:
        bt_audio_stop_discovery();
        ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
        return true;
    default:
        return false;
    }
}

const ui_page_t ui_page_bt = {
    .name = "bt",
    .enter = bt_enter,
    .leave = bt_leave,
    .update = bt_update,
    .draw = bt_draw,
    .input = bt_input,
};

/* ------------------------------------------------------------------ */
/* 5. 设置                                                             */
/* ------------------------------------------------------------------ */
enum {
    SET_VOLUME = 0,
    SET_BRIGHT,
    SET_EQ,
    SET_PLAY_MODE,
    SET_RECONNECT,
    SET_WIFI_AP,
    SET_ABOUT,
    SET_COUNT,
};

static const char *const k_set_names[SET_COUNT] = {
    "音量", "亮度", "音效", "播放模式", "自动连接", "无线传歌",
    "系统信息",
};

static int s_set_sel;
static int s_set_top;

static void set_enter(void)
{
    s_set_sel = 0;
    s_set_top = 0;
}

static void set_value_text(int item, char *buf, size_t n)
{
    const app_settings_t *cfg = app_settings_get();
    switch (item) {
    case SET_VOLUME:
        snprintf(buf, n, "%u%%", (unsigned)(cfg->volume * 100 / 127));
        break;
    case SET_BRIGHT: {
        static const char *lv[] = {"1", "2", "3", "4", "5"};
        uint8_t level = cfg->brightness < 5 ? cfg->brightness : 4;
        snprintf(buf, n, "%s", lv[level]);
        break;
    }
    case SET_EQ: {
        static const char *eq[] = {"平直", "流行", "摇滚", "人声"};
        uint8_t preset = cfg->eq < 4 ? cfg->eq : 0;
        snprintf(buf, n, "%s", eq[preset]);
        break;
    }
    case SET_PLAY_MODE:
        snprintf(buf, n, "%s", cfg->shuffle ? "随机" :
                 cfg->repeat == 2 ? "单曲循环" : "全部循环");
        break;
    case SET_RECONNECT:
        snprintf(buf, n, "%s", cfg->auto_reconnect ? "开" : "关");
        break;
    case SET_WIFI_AP:
        snprintf(buf, n, "%s", net_service_ap_running() ? "开" : "关");
        break;
    case SET_ABOUT:
        snprintf(buf, n, ">");
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void set_draw(ui_canvas_t *c)
{
    /* 滚动窗口跟随选中项 */
    if (s_set_sel < s_set_top) {
        s_set_top = s_set_sel;
    } else if (s_set_sel >= s_set_top + TH_MENU_ROWS) {
        s_set_top = s_set_sel - TH_MENU_ROWS + 1;
    }

    for (int r = 0; r < TH_MENU_ROWS; r++) {
        int i = s_set_top + r;
        if (i >= SET_COUNT) {
            break;
        }
        const int y = TH_MENU_TOP + r * TH_ROW_H;
        const bool sel = (i == s_set_sel);

        char val[20];
        set_value_text(i, val, sizeof(val));
        uiw_menu_row(c, TH_MENU_X, y, TH_MENU_W, TH_ROW_H, k_set_names[i], val, sel, UI_FONT_MED);
    }

    uiw_scrollbar(c, TH_SCROLLBAR_X, TH_MENU_TOP, TH_ROW_H * TH_MENU_ROWS, SET_COUNT, TH_MENU_ROWS, s_set_top);
}

static void set_change(int dir)
{
    app_settings_t cfg = *app_settings_get();
    switch (s_set_sel) {
    case SET_VOLUME:
        app_settings_volume_step((int8_t)dir, 4);
        bt_audio_set_absolute_volume(app_settings_get()->volume);
        break;
    case SET_BRIGHT: {
        int b = (int)cfg.brightness + dir;
        if (b < 0) {
            b = 0;
        }
        if (b > 4) {
            b = 4;
        }
        app_settings_set_brightness((uint8_t)b);
        ssd1306_set_contrast((uint8_t)(0x20 + b * 0x28));
        break;
    }
    case SET_EQ:
        cfg.eq = (uint8_t)((cfg.eq + 4 + dir) % 4);
        app_settings_update(&cfg);
        break;
    case SET_PLAY_MODE: {
        /* 单曲循环 -> 全部循环 -> 随机；反向操作逆序切换。 */
        int mode = cfg.shuffle ? 2 : (cfg.repeat == 2 ? 0 : 1);
        mode = (mode + 3 + dir) % 3;
        cfg.shuffle = mode == 2;
        cfg.repeat = mode == 0 ? 2 : 1;
        app_settings_update(&cfg);
        break;
    }
    case SET_RECONNECT:
        cfg.auto_reconnect = cfg.auto_reconnect ? 0 : 1;
        app_settings_update(&cfg);
        break;
    case SET_WIFI_AP:
        if (net_service_ap_running()) {
            net_service_ap_stop();
        } else {
            net_service_ap_start();
        }
        break;
    default:
        break;
    }
}

static bool set_input(app_key_t key, int32_t value)
{
    (void)value;
    switch (key) {
    case APP_KEY_DOWN:
    case APP_KEY_ROT_CW:
    case APP_KEY_KEY0_SINGLE:
    case APP_KEY_UP:
    case APP_KEY_ROT_CCW: {
        int dir = (key == APP_KEY_DOWN || key == APP_KEY_ROT_CW ||
                   key == APP_KEY_KEY0_SINGLE) ? 1 : -1;
        /* ENTER 之后的箭头方向键改值；这里采用“直接改值”的简化交互 */
        s_set_sel = (s_set_sel + dir + SET_COUNT) % SET_COUNT;
        return true;
    }
    case APP_KEY_ENTER:
    case APP_KEY_PLAY:
    case APP_KEY_KEY0_DOUBLE:
        if (s_set_sel == SET_ABOUT) {
            ui_goto(UI_GO_ABOUT, UI_TRANS_SLIDE_U);
        } else {
            set_change(1);
        }
        return true;
    case APP_KEY_PREV:
    case APP_KEY_KEY0_TRIPLE:
        set_change(-1);
        return true;
    case APP_KEY_NEXT:
        set_change(1);
        return true;
    case APP_KEY_BACK:
        ui_goto(UI_GO_NOW, UI_TRANS_SLIDE_R);
        return true;
    default:
        return false;
    }
}

const ui_page_t ui_page_set = {
    .name = "set",
    .enter = set_enter,
    .update = NULL,
    .draw = set_draw,
    .input = set_input,
};

/* ------------------------------------------------------------------ */
/* 6. 关于 / 系统信息                                                  */
/* ------------------------------------------------------------------ */
static void about_draw(ui_canvas_t *c)
{
    /* The FAT free-space query can be transiently stale (and some cards
     * report a tiny/zero partition value while they are still settling).
     * The CSD capacity is the stable physical card size, so prefer it for
     * the diagnostic page and only use FATFS as a fallback. */
    uint64_t sd_total = bsp_sd_card_capacity_bytes();
    if (sd_total == 0) {
        bsp_sd_info(&sd_total, NULL);
    }
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t heap_free = esp_get_free_heap_size();

    char v[24];

    snprintf(v, sizeof(v), "%uM", (unsigned)(psram_free / (1024 * 1024)));
    uiw_kv_row(c, TH_PAGE_MARGIN, TH_MENU_TOP, TH_CONTENT_W, "内存", v, UI_FONT_MED);

    snprintf(v, sizeof(v), "%uK", (unsigned)(heap_free / 1024));
    uiw_kv_row(c, TH_PAGE_MARGIN, TH_MENU_TOP + TH_ROW_H, TH_CONTENT_W, "堆", v, UI_FONT_MED);

    /* A mounted card can report a transient FATFS query error. bsp_sd_info()
     * still returns its CSD capacity in that case, so prefer a valid byte
     * count over the status code for this display. */
    if (sd_total > 0) {
        /* Decimal GB matches the capacity printed on SD cards. Keep one
         * decimal place for cards >= 1 GB; show MB for smaller media so a
         * valid card can never be rendered as 0G. */
        const uint64_t gb = 1000000000ULL;
        if (sd_total >= gb) {
            uint64_t tenths_gb = (sd_total + 50000000ULL) / 100000000ULL;
            snprintf(v, sizeof(v), "%u.%uG", (unsigned)(tenths_gb / 10),
                     (unsigned)(tenths_gb % 10));
        } else {
            uint64_t mb = (sd_total + 500000ULL) / 1000000ULL;
            if (mb == 0) {
                mb = 1;
            }
            snprintf(v, sizeof(v), "%uM", (unsigned)mb);
        }
    } else if (bsp_sd_is_mounted()) {
        snprintf(v, sizeof(v), "读取失败");
    } else {
        snprintf(v, sizeof(v), "无卡");
    }
    uiw_kv_row(c, TH_PAGE_MARGIN, TH_MENU_TOP + 2 * TH_ROW_H, TH_CONTENT_W, "存储卡", v, UI_FONT_MED);

    int last_y = TH_MENU_TOP + 3 * TH_ROW_H;
    if (net_service_ap_running()) {
        snprintf(v, sizeof(v), "%s", net_service_ap_ip());
        ui_text_ellipsis(c, TH_PAGE_MARGIN, last_y + 2, TH_CONTENT_W, v, UI_FONT_SMALL);
    } else {
        uiw_kv_row(c, TH_PAGE_MARGIN, last_y, TH_CONTENT_W, "热点", "关闭", UI_FONT_MED);
    }
}

static bool about_input(app_key_t key, int32_t value)
{
    (void)value;
    if (key == APP_KEY_BACK || key == APP_KEY_KEY0_SINGLE ||
        key == APP_KEY_KEY0_DOUBLE || key == APP_KEY_KEY0_TRIPLE) {
        ui_goto(UI_GO_SETTINGS, UI_TRANS_SLIDE_U);
        return true;
    }
    return false;
}

const ui_page_t ui_page_about = {
    .name = "about",
    .enter = NULL,
    .update = NULL,
    .draw = about_draw,
    .input = about_input,
};
