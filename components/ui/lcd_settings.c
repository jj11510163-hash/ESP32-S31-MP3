#include "lcd_settings.h"

#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "bt_audio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_update.h"
#include "lcd_font.h"
#include "lcd_gfx.h"
#include "lcd_panel.h"
#include "music.h"
#include "net_service.h"

#define ROWS 5
#define ROW_Y 56
#define ROW_H 52
#define SET_COUNT 11

typedef enum { VIEW_LIST, VIEW_TRANSFER, VIEW_SYSTEM, VIEW_UPDATE } view_t;
typedef enum {
    SET_VOLUME,
    SET_BRIGHT,
    SET_PLAY_MODE,
    SET_RECONNECT,
    SET_WIFI_AP,
    SET_TRANSFER_INFO,
    SET_SLEEP,
    SET_BT_DEVICES,
    SET_BT_CODEC,
    SET_SYSTEM_INFO,
    SET_FW_UPDATE,
} setting_t;

static const char *const s_names[SET_COUNT] = {
    "音量", "屏幕亮度", "播放模式", "开机自动回连", "无线传歌",
    "传歌信息", "息屏时间", "蓝牙设备", "蓝牙编码", "系统信息", "固件更新",
};
static const uint8_t s_sleep_seconds[] = {0, 15, 30, 60, 120};

static view_t s_view;
static int s_selected;
static int s_top;
static bool s_edit;
static bool s_volume_drag;
static uint8_t s_drag_sent_volume;
static int64_t s_drag_sent_us;
static char s_notice[40];
static int64_t s_notice_until_us;
static fw_update_info_t s_update_info;
static bool s_update_confirm;
static int64_t s_update_poll_us;

static void notice(const char *message)
{
    snprintf(s_notice, sizeof(s_notice), "%s", message);
    s_notice_until_us = esp_timer_get_time() + 2200000;
}

static int playback_mode(const app_settings_t *cfg)
{
    return cfg->shuffle ? 2 : cfg->repeat == 2 ? 1 : 0;
}

static void set_playback_mode(int mode)
{
    app_settings_t cfg = *app_settings_get();
    if (mode == playback_mode(&cfg)) return;
    cfg.shuffle = mode == 2;
    cfg.repeat = mode == 1 ? 2 : 1;
    if (app_settings_update(&cfg) != ESP_OK) notice("播放模式保存失败");
}

static int sleep_index(uint8_t seconds)
{
    for (int i = 0; i < (int)sizeof(s_sleep_seconds); i++) {
        if (s_sleep_seconds[i] == seconds) return i;
    }
    return 0;
}

static void set_sleep(int index)
{
    if (index < 0 || index >= (int)sizeof(s_sleep_seconds)) return;
    app_settings_t cfg = *app_settings_get();
    cfg.screen_timeout_s = s_sleep_seconds[index];
    if (app_settings_update(&cfg) != ESP_OK) notice("息屏时间保存失败");
}

static void set_bt_codec(uint8_t codec)
{
    app_settings_t cfg = *app_settings_get();
    if (codec == cfg.bt_codec) return;
    cfg.bt_codec = codec;
    if (app_settings_update(&cfg) != ESP_OK || bt_audio_set_codec_pref(codec) != ESP_OK) {
        notice("蓝牙编码保存失败");
    } else if (bt_audio_state() == BT_ST_CONNECTED || bt_audio_state() == BT_ST_STREAMING) {
        notice("重连耳机后生效");
    }
}

static void volume_step(int dir)
{
    int value = (int)app_settings_get()->volume + dir * 6;
    if (value < 0) value = 0;
    if (value > 127) value = 127;
    if (value == app_settings_get()->volume) return;
    app_settings_set_volume((uint8_t)value);
    bt_audio_set_absolute_volume((uint8_t)value);
}

static uint8_t volume_at_x(uint16_t x)
{
    int px = x < 286 ? 286 : x > 418 ? 418 : x;
    return (uint8_t)((px - 286) * 127 / 132);
}

static void set_brightness(uint8_t value)
{
    if (value == app_settings_get()->brightness && lcd_panel_brightness_available()) return;
    if (lcd_panel_set_brightness(value) != ESP_OK) {
        s_edit = false;
        notice("背光调节不可用");
    } else if (app_settings_set_brightness(value) != ESP_OK) {
        notice("亮度保存失败");
    }
}

static void brightness_step(int dir)
{
    int value = (int)app_settings_get()->brightness + dir;
    if (value < 0) value = 0;
    if (value > 4) value = 4;
    set_brightness((uint8_t)value);
}

static void change_setting(int index, int dir)
{
    app_settings_t cfg = *app_settings_get();
    esp_err_t err = ESP_OK;
    switch (index) {
    case SET_VOLUME:
        volume_step(dir);
        return;
    case SET_BRIGHT:
        brightness_step(dir);
        return;
    case SET_PLAY_MODE:
        set_playback_mode((playback_mode(&cfg) + 3 + dir) % 3);
        return;
    case SET_RECONNECT:
        cfg.auto_reconnect = !cfg.auto_reconnect;
        err = app_settings_update(&cfg);
        break;
    case SET_WIFI_AP:
        err = net_service_ap_running() ? net_service_ap_stop() : net_service_ap_start();
        break;
    case SET_SLEEP:
        set_sleep((sleep_index(cfg.screen_timeout_s) + 5 + dir) % 5);
        return;
    case SET_BT_CODEC:
        set_bt_codec((cfg.bt_codec + 3 + dir) % 3);
        return;
    default:
        return;
    }
    if (err != ESP_OK) notice(index == SET_WIFI_AP ? "无线传歌切换失败" : "设置保存失败");
}

static void clamp_selection(void)
{
    if (s_selected < 0) s_selected = 0;
    if (s_selected >= SET_COUNT) s_selected = SET_COUNT - 1;
    if (s_selected < s_top) s_top = s_selected;
    if (s_selected >= s_top + ROWS) s_top = s_selected - ROWS + 1;
    if (s_top < 0) s_top = 0;
    if (s_top > SET_COUNT - ROWS) s_top = SET_COUNT - ROWS;
}

void lcd_settings_enter(void)
{
    s_view = VIEW_LIST;
    s_selected = 0;
    s_top = 0;
    s_edit = false;
    s_volume_drag = false;
    s_notice[0] = '\0';
    memset(&s_update_info, 0, sizeof(s_update_info));
    s_update_confirm = false;
    s_update_poll_us = 0;
}

bool lcd_settings_tick(void)
{
    bool changed = false;
    int64_t now = esp_timer_get_time();
    if (s_notice[0] && now >= s_notice_until_us) {
        s_notice[0] = '\0';
        changed = true;
    }
    if (s_view == VIEW_UPDATE && now - s_update_poll_us >= 120000) {
        s_update_poll_us = now;
        fw_update_info_t next;
        fw_update_get_status(&next);
        if (next.state != s_update_info.state ||
            next.percent != s_update_info.percent ||
            next.image_size != s_update_info.image_size ||
            next.available != s_update_info.available ||
            strcmp(next.current_version, s_update_info.current_version) != 0 ||
            strcmp(next.candidate_version, s_update_info.candidate_version) != 0 ||
            strcmp(next.message, s_update_info.message) != 0) {
            s_update_info = next;
            changed = true;
        }
    }
    return changed;
}

static void draw_back_and_title(const char *title)
{
    lcd_line(39, 30, 20, 30, 2, C_INK);
    lcd_line(20, 30, 29, 21, 2, C_INK);
    lcd_line(20, 30, 29, 39, 2, C_INK);
    lcd_text(60, 14, title, 24, C_INK);
    lcd_rect(16, 51, 448, 1, C_BORDER);
}

static void draw_toggle(int y, bool enabled)
{
    lcd_round_rect(400, y + 10, 58, 32, 16, enabled ? C_TEAL : C_BORDER);
    lcd_circle(enabled ? 439 : 419, y + 26, 12, C_CARD);
}

static void draw_chevron(int x, int y)
{
    lcd_line(x - 4, y - 8, x + 4, y, 2, C_MUTED);
    lcd_line(x + 4, y, x - 4, y + 8, 2, C_MUTED);
}

static void draw_volume(int y, const app_settings_t *cfg)
{
    char value[12];
    snprintf(value, sizeof(value), "%u%%", (unsigned)(cfg->volume * 100 + 63) / 127);
    lcd_text(181, y + 13, value, 18, C_MUTED);
    lcd_line(254, y + 26, 268, y + 26, 2, C_INK);
    lcd_round_rect(286, y + 24, 132, 5, 2, C_BORDER);
    int fill = (int)((uint32_t)cfg->volume * 132 / 127);
    if (fill) lcd_round_rect(286, y + 24, fill, 5, 2, C_TEAL);
    lcd_circle(286 + fill, y + 26, 7, C_TEAL);
    lcd_line(440, y + 26, 454, y + 26, 2, C_INK);
    lcd_line(447, y + 19, 447, y + 33, 2, C_INK);
}

static void draw_brightness(int y, const app_settings_t *cfg)
{
    if (!lcd_panel_brightness_available()) {
        lcd_text_center(290, y + 14, 164, "不可用", 18, C_MUTED);
        return;
    }
    for (int i = 0; i < 5; i++) {
        int x = 294 + i * 32;
        int height = 10 + i * 5;
        lcd_round_rect(x, y + 40 - height, 22, height, 3,
                       i <= cfg->brightness ? C_TEAL : C_BORDER);
    }
}

static void draw_mode(int y, const app_settings_t *cfg)
{
    static const char *const labels[3] = {"全部", "单曲", "随机"};
    int selected = playback_mode(cfg);
    for (int i = 0; i < 3; i++) {
        int x = 226 + i * 78;
        lcd_round_rect(x, y + 7, 74, 38, 5, i == selected ? C_TEAL : C_CARD);
        lcd_text_center(x, y + 14, 74, labels[i], 18,
                        i == selected ? C_CARD : C_MUTED);
    }
}

static void draw_bt_codec(int y, uint8_t codec)
{
    static const char *const labels[3] = {"自动", "SBC", "AAC"};
    for (int i = 0; i < 3; i++) {
        int x = 226 + i * 78;
        lcd_round_rect(x, y + 7, 74, 38, 5, i == codec ? C_TEAL : C_CARD);
        lcd_text_center(x, y + 14, 74, labels[i], 18,
                        i == codec ? C_CARD : C_MUTED);
    }
}

static void draw_sleep(int y, uint8_t seconds)
{
    char value[20];
    if (seconds) snprintf(value, sizeof(value), "%u秒", seconds);
    else snprintf(value, sizeof(value), "常亮");
    lcd_line(315, y + 26, 322, y + 19, 2, C_MUTED);
    lcd_line(315, y + 26, 322, y + 33, 2, C_MUTED);
    lcd_text_center(334, y + 14, 84, value, 18, C_TEAL);
    draw_chevron(436, y + 26);
}

static void draw_row(int index, int y)
{
    const app_settings_t *cfg = app_settings_get();
    bool selected = index == s_selected;
    if (selected) lcd_round_rect(12, y + 1, 456, 50, 6, C_SOFT);
    else lcd_rect(20, y + ROW_H - 1, 444, 1, C_BORDER);
    lcd_text_ellipsis(20, y + 10, 205, s_names[index], 24, C_INK);
    switch (index) {
    case SET_VOLUME: draw_volume(y, cfg); break;
    case SET_BRIGHT: draw_brightness(y, cfg); break;
    case SET_PLAY_MODE: draw_mode(y, cfg); break;
    case SET_RECONNECT: draw_toggle(y, cfg->auto_reconnect); break;
    case SET_WIFI_AP: draw_toggle(y, net_service_ap_running()); break;
    case SET_TRANSFER_INFO:
        lcd_text_ellipsis(220, y + 14, 211,
                          net_service_ap_running() ? net_service_ap_ssid() : "热点未开启",
                          18, C_MUTED);
        draw_chevron(446, y + 26);
        break;
    case SET_SLEEP: draw_sleep(y, cfg->screen_timeout_s); break;
    case SET_BT_CODEC: draw_bt_codec(y, cfg->bt_codec); break;
    case SET_BT_DEVICES: {
        bt_state_t state = bt_audio_state();
        const char *name = state == BT_ST_CONNECTING ? "连接中" :
                           state == BT_ST_CONNECTED || state == BT_ST_STREAMING ?
                           bt_audio_connected_name() : "未连接";
        if (!name[0]) name = "已连接";
        lcd_text_ellipsis(222, y + 14, 210,
                          name, 18, C_MUTED);
        draw_chevron(446, y + 26);
        break;
    }
    case SET_SYSTEM_INFO:
    case SET_FW_UPDATE:
        draw_chevron(446, y + 26);
        break;
    }
    if (selected && s_edit) {
        lcd_rect(228, y + 4, 2, 44, C_TEAL);
        lcd_rect(464, y + 4, 2, 44, C_TEAL);
    }
}

static void draw_list(void)
{
    clamp_selection();
    draw_back_and_title("设置");
    char progress[24];
    snprintf(progress, sizeof(progress), "%d/%d", s_selected + 1, SET_COUNT);
    lcd_text_center(370, 18, 94, progress, 18, C_MUTED);
    for (int row = 0; row < ROWS; row++) {
        int index = s_top + row;
        if (index < SET_COUNT) draw_row(index, ROW_Y + row * ROW_H);
    }
    int track_height = ROWS * ROW_H;
    int thumb_height = track_height * ROWS / SET_COUNT;
    int thumb_y = ROW_Y + (track_height - thumb_height) * s_top / (SET_COUNT - ROWS);
    lcd_round_rect(473, ROW_Y, 3, track_height, 1, C_BORDER);
    lcd_round_rect(473, thumb_y, 3, thumb_height, 1, C_TEAL);
    if (s_notice[0] && esp_timer_get_time() < s_notice_until_us) {
        lcd_round_rect(118, 265, 244, 38, 5, C_INK);
        lcd_text_center(118, 271, 244, s_notice, 18, C_CARD);
    }
}

static void detail_row(int y, const char *label, const char *value)
{
    lcd_text(20, y, label, 18, C_MUTED);
    lcd_text_ellipsis(178, y, 282, value, 18, C_INK);
    lcd_rect(20, y + 39, 444, 1, C_BORDER);
}

static void format_bytes(char *out, size_t size, uint64_t bytes)
{
    if (bytes >= 1000000000ULL) {
        uint64_t tenths = (bytes + 50000000ULL) / 100000000ULL;
        snprintf(out, size, "%llu.%llu GB", (unsigned long long)(tenths / 10),
                 (unsigned long long)(tenths % 10));
    } else {
        snprintf(out, size, "%llu MB",
                 (unsigned long long)((bytes + 500000ULL) / 1000000ULL));
    }
}

static void draw_transfer(void)
{
    draw_back_and_title("传歌信息");
    bool on = net_service_ap_running();
    detail_row(65, "热点状态", on ? "已开启" : "已关闭");
    detail_row(107, "WiFi 名称", on ? net_service_ap_ssid() : "热点未开启");
    detail_row(149, "WiFi 密码", NET_AP_PASSWORD);
    detail_row(191, "访问地址", on ? net_service_ap_ip() : "开启热点后可访问");
    char clients[18];
    snprintf(clients, sizeof(clients), "%u", on ? net_service_clients() : 0);
    detail_row(233, "连接设备", clients);
}

static void draw_system(void)
{
    draw_back_and_title("系统信息");
    char value[32];
    uint64_t total = bsp_sd_card_capacity_bytes();
    uint64_t fat_total = 0;
    uint64_t free_bytes = 0;
    esp_err_t sd_err = bsp_sd_is_mounted() ?
                       bsp_sd_info(&fat_total, &free_bytes) : ESP_ERR_INVALID_STATE;
    if (!total) total = fat_total;
    if (total) format_bytes(value, sizeof(value), total);
    else snprintf(value, sizeof(value), "%s", bsp_sd_is_mounted() ? "读取失败" : "未插入");
    detail_row(60, "存储卡", value);
    if (sd_err == ESP_OK) format_bytes(value, sizeof(value), free_bytes);
    else snprintf(value, sizeof(value), "%s", bsp_sd_is_mounted() ? "读取失败" : "-");
    detail_row(101, "剩余空间", value);
    if (music_lib_scanning()) snprintf(value, sizeof(value), "扫描中");
    else snprintf(value, sizeof(value), "%d 首", music_lib_count());
    detail_row(142, "曲库", value);
    snprintf(value, sizeof(value), "%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    detail_row(183, "内部可用", value);
    snprintf(value, sizeof(value), "%u MB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)));
    detail_row(224, "PSRAM 可用", value);
    detail_row(265, "蓝牙编码", bt_audio_active_codec_name());
}

static void draw_update(void)
{
    draw_back_and_title("固件更新");
    detail_row(60, "当前版本", s_update_info.current_version[0] ?
               s_update_info.current_version : "读取中");
    detail_row(102, "SD 卡版本", s_update_info.candidate_version[0] ?
               s_update_info.candidate_version : "-");
    char size[32];
    if (s_update_info.image_size) {
        uint32_t tenths = (s_update_info.image_size + 50000U) / 100000U;
        snprintf(size, sizeof(size), "%lu.%lu MB", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    } else snprintf(size, sizeof(size), "-");
    detail_row(144, "更新包大小", size);

    const char *state = "待检查";
    switch (s_update_info.state) {
    case FW_UPDATE_IDLE: state = s_update_info.available ? "准备就绪" : "不可更新"; break;
    case FW_UPDATE_PREPARING: state = "准备中"; break;
    case FW_UPDATE_RUNNING: state = "更新中"; break;
    case FW_UPDATE_SUCCESS: state = "已完成"; break;
    case FW_UPDATE_FAILED: state = "更新失败"; break;
    }
    lcd_text(20, 188, "状态", 18, C_MUTED);
    lcd_text(178, 188, state, 18, s_update_info.state == FW_UPDATE_FAILED ? 0xB3453C : C_INK);
    if (s_update_info.state == FW_UPDATE_RUNNING || s_update_info.state == FW_UPDATE_SUCCESS) {
        char percent[8];
        snprintf(percent, sizeof(percent), "%u%%", (unsigned)s_update_info.percent);
        lcd_text(460 - lcd_text_width(percent, 18), 188, percent, 18, C_TEAL);
    }
    const char *message = s_update_confirm ? "确认安装？更新时会停止播放并重启" :
                          (s_update_info.state == FW_UPDATE_IDLE && s_update_info.available ?
                           "SD 卡路径：/update/firmware.bin" : s_update_info.message);
    lcd_text_ellipsis(20, 215, 440, message, 18, C_MUTED);

    lcd_round_rect(20, 245, 440, 7, 3, C_BORDER);
    if (s_update_info.percent) {
        int width = (int)s_update_info.percent * 440 / 100;
        lcd_round_rect(20, 245, width, 7, 3, C_TEAL);
    }
    const char *action = fw_update_busy() ?
                         (s_update_info.state == FW_UPDATE_SUCCESS ? "即将重启" : "正在更新") :
                         (s_update_info.available && s_update_info.state == FW_UPDATE_IDLE ?
                          (s_update_confirm ? "确认安装" : "安装此版本") : "重新检查");
    bool enabled = !fw_update_busy();
    lcd_round_rect(20, 265, 440, 42, 5, enabled ? C_TEAL : C_BORDER);
    lcd_text_center(20, 275, 440, action, 18, enabled ? C_CARD : C_MUTED);
}

void lcd_settings_draw(uint16_t *frame)
{
    lcd_gfx_bind(frame);
    lcd_clear(C_BG);
    if (s_view == VIEW_TRANSFER) draw_transfer();
    else if (s_view == VIEW_SYSTEM) draw_system();
    else if (s_view == VIEW_UPDATE) draw_update();
    else draw_list();
}

bool lcd_settings_draw_volume(uint16_t *frame, int *top)
{
    if (!frame || s_view != VIEW_LIST || SET_VOLUME < s_top ||
        SET_VOLUME >= s_top + ROWS) return false;
    const int y = ROW_Y + (SET_VOLUME - s_top) * ROW_H;
    lcd_gfx_bind(frame);
    lcd_rect(181, y + 7, 281, 40, s_selected == SET_VOLUME ? C_SOFT : C_BG);
    draw_volume(y, app_settings_get());
    if (s_selected == SET_VOLUME && s_edit)
        lcd_rect(228, y + 7, 2, 40, C_TEAL);
    if (top) *top = y + 7;
    return true;
}

static lcd_settings_result_t activate_selected(void)
{
    if (s_selected == SET_BRIGHT && !lcd_panel_brightness_available()) {
        notice("背光调节不可用");
    } else if (s_selected == SET_VOLUME || s_selected == SET_BRIGHT) {
        s_edit = !s_edit;
    } else if (s_selected == SET_TRANSFER_INFO) {
        s_view = VIEW_TRANSFER;
    } else if (s_selected == SET_BT_DEVICES) {
        return LCD_SETTINGS_GO_BT;
    } else if (s_selected == SET_SYSTEM_INFO) {
        s_view = VIEW_SYSTEM;
    } else if (s_selected == SET_FW_UPDATE) {
        s_view = VIEW_UPDATE;
        s_update_confirm = false;
        fw_update_probe(&s_update_info);
    } else {
        change_setting(s_selected, 1);
    }
    return LCD_SETTINGS_STAY;
}

static lcd_settings_result_t go_back(void)
{
    if (s_view == VIEW_UPDATE && fw_update_busy()) return LCD_SETTINGS_STAY;
    if (s_view == VIEW_UPDATE && s_update_confirm) {
        s_update_confirm = false;
        return LCD_SETTINGS_STAY;
    }
    if (s_edit) s_edit = false;
    else if (s_view != VIEW_LIST) s_view = VIEW_LIST;
    else return LCD_SETTINGS_BACK;
    return LCD_SETTINGS_STAY;
}

static lcd_settings_result_t update_activate(void)
{
    fw_update_get_status(&s_update_info);
    if (fw_update_busy()) return LCD_SETTINGS_STAY;
    if (s_update_info.state != FW_UPDATE_IDLE || !s_update_info.available) {
        s_update_confirm = false;
        fw_update_probe(&s_update_info);
    } else if (!s_update_confirm) {
        s_update_confirm = true;
    } else {
        s_update_confirm = false;
        fw_update_start();
        fw_update_get_status(&s_update_info);
    }
    return LCD_SETTINGS_STAY;
}

lcd_settings_result_t lcd_settings_key(app_key_t key)
{
    if (s_view == VIEW_UPDATE) {
        if (fw_update_busy()) return LCD_SETTINGS_STAY;
        switch (key) {
        case APP_KEY_KEY0_LONG:
        case APP_KEY_BACK:
        case APP_KEY_PAGE_PREV:
            return go_back();
        case APP_KEY_KEY0_QUAD:
        case APP_KEY_PAGE_NEXT:
            if (s_update_confirm) return go_back();
            return LCD_SETTINGS_GO_HOME;
        case APP_KEY_KEY0_SINGLE:
        case APP_KEY_KEY0_DOUBLE:
        case APP_KEY_ENTER:
        case APP_KEY_PLAY:
            return update_activate();
        case APP_KEY_KEY0_TRIPLE:
            s_update_confirm = false;
            fw_update_probe(&s_update_info);
            return LCD_SETTINGS_STAY;
        default:
            return LCD_SETTINGS_STAY;
        }
    }
    if (key == APP_KEY_KEY0_QUAD || key == APP_KEY_PAGE_NEXT)
        return LCD_SETTINGS_GO_HOME;
    if (key == APP_KEY_PAGE_PREV)
        return LCD_SETTINGS_GO_BT;
    if (key == APP_KEY_KEY0_LONG || key == APP_KEY_BACK)
        return go_back();
    if (s_view != VIEW_LIST) return LCD_SETTINGS_STAY;

    switch (key) {
    case APP_KEY_KEY0_SINGLE:
    case APP_KEY_DOWN:
    case APP_KEY_ROT_CW:
        if (s_edit) change_setting(s_selected, 1);
        else { s_selected = (s_selected + 1) % SET_COUNT; clamp_selection(); }
        break;
    case APP_KEY_KEY0_TRIPLE:
    case APP_KEY_UP:
    case APP_KEY_ROT_CCW:
        if (s_edit) change_setting(s_selected, -1);
        else { s_selected = (s_selected + SET_COUNT - 1) % SET_COUNT; clamp_selection(); }
        break;
    case APP_KEY_KEY0_DOUBLE:
    case APP_KEY_ENTER:
    case APP_KEY_PLAY:
        return activate_selected();
    case APP_KEY_NEXT: change_setting(s_selected, 1); break;
    case APP_KEY_PREV: change_setting(s_selected, -1); break;
    default: break;
    }
    return LCD_SETTINGS_STAY;
}

lcd_settings_result_t lcd_settings_touch(uint16_t x, uint16_t y)
{
    s_volume_drag = false;
    if (y <= 52) {
        if (x < 62) return go_back();
        return LCD_SETTINGS_STAY;
    }
    if (s_view == VIEW_UPDATE) {
        if (x >= 20 && x < 460 && y >= 265 && y < 307)
            return update_activate();
        return LCD_SETTINGS_STAY;
    }
    if (s_view != VIEW_LIST || y < ROW_Y || y >= ROW_Y + ROWS * ROW_H)
        return LCD_SETTINGS_STAY;
    int index = s_top + (y - ROW_Y) / ROW_H;
    if (index >= SET_COUNT) return LCD_SETTINGS_STAY;
    s_selected = index;
    s_edit = false;
    if (index == SET_VOLUME) {
        if (x < 280 && x >= 236) volume_step(-1);
        else if (x >= 426) volume_step(1);
        else if (x >= 286) {
            s_volume_drag = true;
            uint8_t value = volume_at_x(x);
            if (value != app_settings_get()->volume) {
                app_settings_set_volume(value);
                bt_audio_set_absolute_volume(value);
            }
            s_drag_sent_volume = value;
            s_drag_sent_us = esp_timer_get_time();
        } else s_edit = true;
    } else if (index == SET_BRIGHT) {
        if (!lcd_panel_brightness_available()) notice("背光调节不可用");
        else if (x >= 288) {
            int step = ((int)x - 288) / 32;
            if (step > 4) step = 4;
            set_brightness((uint8_t)step);
        } else s_edit = true;
    } else if (index == SET_PLAY_MODE) {
        if (x >= 226) {
            int mode = ((int)x - 226) / 78;
            if (mode > 2) mode = 2;
            set_playback_mode(mode);
        } else change_setting(index, 1);
    } else if (index == SET_BT_CODEC) {
        if (x >= 226) {
            int codec = ((int)x - 226) / 78;
            if (codec > 2) codec = 2;
            set_bt_codec((uint8_t)codec);
        } else change_setting(index, 1);
    } else if (index == SET_SLEEP) {
        change_setting(index, x >= 292 && x < 336 ? -1 : 1);
    } else if (index == SET_RECONNECT || index == SET_WIFI_AP) {
        change_setting(index, 1);
    } else {
        return activate_selected();
    }
    return LCD_SETTINGS_STAY;
}

bool lcd_settings_touch_begin(uint16_t x, uint16_t y)
{
    if (s_view != VIEW_LIST || x < 286 || x >= 426 ||
        y < ROW_Y || y >= ROW_Y + ROWS * ROW_H) return false;
    int index = s_top + (y - ROW_Y) / ROW_H;
    if (index != SET_VOLUME) return false;
    lcd_settings_touch(x, y);
    return s_volume_drag;
}

void lcd_settings_scroll(int rows)
{
    if (s_view != VIEW_LIST || !rows) return;
    s_edit = false;
    s_top += rows;
    if (s_top < 0) s_top = 0;
    if (s_top > SET_COUNT - ROWS) s_top = SET_COUNT - ROWS;
    s_selected = s_top;
}

bool lcd_settings_touch_move(uint16_t x, uint16_t y)
{
    (void)y;
    if (!s_volume_drag || s_view != VIEW_LIST) return false;
    uint8_t value = volume_at_x(x);
    if (value == app_settings_get()->volume) return false;
    app_settings_set_volume(value);
    int64_t now = esp_timer_get_time();
    if (now - s_drag_sent_us >= 150000) {
        bt_audio_set_absolute_volume(value);
        s_drag_sent_volume = value;
        s_drag_sent_us = now;
    }
    return true;
}

void lcd_settings_touch_end(void)
{
    if (s_volume_drag && s_drag_sent_volume != app_settings_get()->volume)
        bt_audio_set_absolute_volume(app_settings_get()->volume);
    s_volume_drag = false;
}
