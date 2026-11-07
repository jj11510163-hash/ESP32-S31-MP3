/* LCD playback, music library, Bluetooth and settings pages. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "app_core.h"
#include "bt_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lcd_cover.h"
#include "lcd_font.h"
#include "lcd_gfx.h"
#include "lcd_lyrics.h"
#include "lcd_panel.h"
#include "lcd_settings.h"
#include "lcd_touch.h"
#include "music.h"

static const char *TAG = "lcd_home";
extern const uint8_t mode_icons_bin_start[] asm("_binary_mode_icons_bin_start");
#define LIB_ROWS 4
#define LIB_ROW_Y 56
#define LIB_ROW_H 64
#define CATEGORY_ROWS 4
#define CATEGORY_ROW_Y 104
#define CATEGORY_ROW_H 53
#define CATEGORY_MODES 5
#define BT_ROWS 3
#define BT_ROW_Y 160
#define BT_ROW_H 53
#define C_WARN 0xA64E49
#define TOUCH_SWIPE_MIN 28
#define TOUCH_PAGE_MIN 60
#define HOME_TITLE_X 18
#define HOME_TITLE_Y 238
#define HOME_TITLE_W 214
#define HOME_PROGRESS_H 28
#define UI_FRAME_US 33333
#define THUMB_IDLE_US 180000
#define HOME_TITLE_FRAME_US UI_FRAME_US
#define LYRIC_X 256
#define LYRIC_Y 26
#define LYRIC_W 198
#define LYRIC_H 168
#define LYRIC_SCROLL_US 300000
#define LYRIC_SCROLL_PX 24

typedef enum {
    PAGE_HOME, PAGE_LIBRARY, PAGE_CATEGORY, PAGE_FAVORITES, PAGE_BT,
    PAGE_SETTINGS,
} ui_page_t;
typedef enum {
    CATEGORY_TITLE, CATEGORY_ARTIST, CATEGORY_ALBUM, CATEGORY_PUBLISHER,
    CATEGORY_FOLDER,
} category_mode_t;
typedef struct {
    char *key;
    int count;
    int last_index;
} category_group_t;
typedef enum { TOUCH_CAPTURE_NONE, TOUCH_CAPTURE_VOLUME, TOUCH_CAPTURE_SEEK,
               TOUCH_CAPTURE_WAKE } touch_capture_t;
typedef struct {
    lcd_touch_event_t event;
    uint16_t x;
    uint16_t y;
    bool cancelled;
} touch_sample_t;

static uint16_t *s_frame;
static bool s_panel;
static bool s_dirty = true;
static bool s_home_progress_dirty;
static bool s_settings_volume_dirty;
static const char *s_last_lyric_previous;
static const char *s_last_lyric_current;
static const char *s_last_lyric_translation;
static const char *s_last_lyric_next;
static uint16_t *s_lyric_frames;
static bool s_lyric_scrolling;
static int64_t s_lyric_scroll_start_us;
static int64_t s_last_lyric_frame_us;
static uint32_t s_last_lyric_pos;
static ui_page_t s_page;
static int s_lib_top;
static int s_lib_selected;
static category_mode_t s_category_mode = CATEGORY_ARTIST;
static category_group_t *s_category_groups;
static int s_category_group_count;
static int s_category_group_capacity;
static int s_category_selected = -1;
static int s_category_top;
static bool s_category_in_group;
static char s_category_key[MUSIC_PATH_MAX];
static int *s_category_tracks;
static int s_category_track_count;
static int s_category_track_capacity;
static int s_category_track_selected;
static int s_category_track_top;
static int *s_favorite_tracks;
static int s_favorite_count;
static int s_favorite_capacity;
static int s_favorite_selected;
static int s_favorite_top;
static uint32_t s_library_generation;
static char s_music_notice[40];
static int64_t s_music_notice_until_us;
static int s_bt_top;
static int s_bt_selected;
static bool s_bt_scan_pending;
static bool s_bt_cancel_sent;
static bool s_bt_confirm_forget;
static bool s_bt_confirm_yes;
static char s_bt_forget_addr[18];
static char s_bt_forget_name[32];
static char s_bt_notice[40];
static int64_t s_bt_notice_until_us;
static bt_forget_status_t s_bt_last_forget_status;
static char s_track_path[MUSIC_PATH_MAX];
static bool s_track_assets_pending;
static QueueHandle_t s_touch_queue;
static bool s_touch_down;
static uint16_t s_touch_start_x;
static uint16_t s_touch_start_y;
static uint16_t s_touch_last_x;
static uint16_t s_touch_last_y;
static ui_page_t s_touch_page;
static touch_capture_t s_touch_capture;
static uint32_t s_seek_preview_ms;
static uint8_t s_bt_anim_phase;
static int64_t s_bt_anim_us;
static int64_t s_scroll_origin_us;
static int64_t s_last_scroll_frame_us;
static int64_t s_last_touch_frame_us;
static int64_t s_last_list_scroll_us;
static int64_t s_last_activity_us;
static bool s_screen_asleep;
static ui_page_t s_settings_return_page;

static void open_bt(void);
static void open_library(void);
static void open_category(void);
static void open_favorites(void);

static void open_settings(void)
{
    s_settings_return_page = s_page;
    lcd_settings_enter();
    s_page = PAGE_SETTINGS;
    s_dirty = true;
}

static void settings_result(lcd_settings_result_t result)
{
    if (result == LCD_SETTINGS_BACK) {
        s_page = s_settings_return_page;
        s_dirty = true;
    } else if (result == LCD_SETTINGS_GO_BT) open_bt();
    else if (result == LCD_SETTINGS_GO_HOME) { s_page = PAGE_HOME; s_dirty = true; }
}

static uint32_t mix(uint32_t a, uint32_t b, int pct)
{
    int r = (((a >> 16) & 255) * (100 - pct) + ((b >> 16) & 255) * pct) / 100;
    int g = (((a >> 8) & 255) * (100 - pct) + ((b >> 8) & 255) * pct) / 100;
    int bl = ((a & 255) * (100 - pct) + (b & 255) * pct) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

static void draw_cover(void)
{
    const uint16_t *art = lcd_cover_pixels();
    if (art) {
        for (int y = 0; y < 212; y++) {
            int dy = y < 16 ? 15 - y : y >= 196 ? y - 196 : 0;
            for (int x = 0; x < 218; x++) {
                int dx = x < 16 ? 15 - x : x >= 202 ? x - 202 : 0;
                if (dx * dx + dy * dy <= 16 * 16)
                    s_frame[(16 + y) * LCD_W + 16 + x] = art[y * 218 + x];
            }
        }
        return;
    }
    /* Deliberate default artwork until a cover source is added. */
    lcd_round_rect(16, 16, 218, 212, 16, C_COVER_A);
    for (int y = 17; y < 227; y++) {
        int edge = 0;
        if (y < 31) edge = 16 - (y - 16);
        else if (y > 212) edge = 16 - (227 - y);
        if (edge < 0) edge = 0;
        int inset = edge > 0 ? (edge * edge) / 16 : 0;
        lcd_rect(17 + inset, y, 216 - 2 * inset, 1,
                 mix(C_COVER_A, C_COVER_B, (y - 17) * 100 / 210));
    }
    /* A graphic record reads as album art, without implying embedded art exists. */
    lcd_circle(125, 120, 77, 0xEAF5F1);
    lcd_circle(125, 120, 68, 0x53968D);
    lcd_circle(125, 120, 59, 0x407F79);
    lcd_circle(125, 120, 46, 0x53968D);
    lcd_circle(125, 120, 32, 0x407F79);
    lcd_circle(125, 120, 20, 0xD8EAE5);
    lcd_circle(125, 120, 5, 0x39746E);
    lcd_circle(165, 79, 15, 0xD6EBE4);
    lcd_text(31, 25, "MUSIC", 18, 0x376C65);
}

static void draw_prev_icon(int cx, int cy, uint32_t color)
{
    lcd_rect(cx - 13, cy - 9, 3, 18, color);
    lcd_triangle(cx + 8, cy - 10, cx + 8, cy + 10, cx - 8, cy, color);
}

static void draw_next_icon(int cx, int cy, uint32_t color)
{
    lcd_triangle(cx - 8, cy - 10, cx - 8, cy + 10, cx + 8, cy, color);
    lcd_rect(cx + 10, cy - 9, 3, 18, color);
}

static void draw_library_icon(void)
{
    for (int y = 0; y < 3; y++) {
        int line_y = 277 + y * 7;
        lcd_circle(205, line_y + 1, 2, C_TEAL);
        lcd_round_rect(211, line_y, 18, 3, 1, C_TEAL);
    }
}

static void draw_bt_icon(int cx, int cy, uint32_t color)
{
    lcd_line(cx, cy - 11, cx, cy + 11, 2, color);
    lcd_line(cx, cy - 11, cx + 8, cy - 5, 2, color);
    lcd_line(cx + 8, cy - 5, cx - 7, cy + 7, 2, color);
    lcd_line(cx - 7, cy - 7, cx + 8, cy + 5, 2, color);
    lcd_line(cx + 8, cy + 5, cx, cy + 11, 2, color);
}

static void draw_settings_icon(int cx, int cy, uint32_t color)
{
    lcd_circle(cx, cy, 10, color);
    lcd_circle(cx, cy, 6, C_BG);
    lcd_circle(cx, cy, 2, color);
    for (int i = -1; i <= 1; i += 2) {
        lcd_rect(cx + i * 11 - 2, cy - 2, 5, 4, color);
        lcd_rect(cx - 2, cy + i * 11 - 2, 4, 5, color);
    }
}

static void time_text(char *out, size_t out_size, uint32_t ms)
{
    unsigned sec = ms / 1000;
    if (sec >= 3600)
        snprintf(out, out_size, "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        snprintf(out, out_size, "%u:%02u", sec / 60, sec % 60);
}

static const char *take_lyric_line(const char *line, char out[96], int size)
{
    const int width = 184;
    if (lcd_text_width(line, size) <= width) {
        snprintf(out, 96, "%s", line);
        return line + strlen(line);
    }

    /* Prefer a natural break while preserving UTF-8 character boundaries. */
    size_t bytes = 0, natural_break = 0;
    const char *p = line;
    while (*p) {
        const char *next = p + 1;
        while (((unsigned char)*next & 0xC0) == 0x80) next++;
        size_t candidate = (size_t)(next - line);
        if (candidate >= 96) break;
        memcpy(out, line, candidate);
        out[candidate] = '\0';
        int text_width = lcd_text_width(out, size);
        if (text_width > width) break;
        bytes = candidate;
        if (text_width >= width / 2 &&
            (*p == ' ' || *p == ',' || *p == '.' ||
             strncmp(p, "，", 3) == 0 || strncmp(p, "。", 3) == 0 ||
             strncmp(p, "、", 3) == 0 || strncmp(p, "！", 3) == 0 ||
             strncmp(p, "？", 3) == 0))
            natural_break = candidate;
        p = next;
    }
    if (!bytes) {
        out[0] = '\0';
        return line;
    }
    if (natural_break) bytes = natural_break;
    out[bytes] = '\0';
    p = line + bytes;
    while (*p == ' ') p++;
    return p;
}

static void draw_active_lyric(const char *line)
{
    const int x = 263, width = 184;
    if (!line[0]) line = "♪";
    char first[96], second[96];
    const char *remaining = take_lyric_line(line, first, 18);
    if (!remaining[0] || remaining == line) {
        lcd_text_ellipsis(x, 99, width, line, 18, C_TEAL);
        return;
    }
    const char *third = take_lyric_line(remaining, second, 18);
    if (!third[0] || third == remaining) {
        lcd_text(x, 84, first, 18, C_TEAL);
        lcd_text_ellipsis(x, 113, width, remaining, 18, C_TEAL);
        return;
    }
    lcd_text(x, 70, first, 18, C_TEAL);
    lcd_text(x, 99, second, 18, C_TEAL);
    lcd_text_ellipsis(x, 128, width, third, 18, C_TEAL);
}

static int wrap_lyric(const char *line, int size, int limit, char out[3][96])
{
    int count = 0;
    while (*line && count < limit) {
        if (count == limit - 1) {
            snprintf(out[count], 96, "%s", line);
            return count + 1;
        }
        const char *next = take_lyric_line(line, out[count], size);
        if (next == line) {
            snprintf(out[count], 96, "%s", line);
            return count + 1;
        }
        count++;
        line = next;
    }
    return count;
}

static void draw_bilingual_lyric(const char *current, const char *translation)
{
    char original_lines[3][96], translated_lines[3][96];
    int original_count = wrap_lyric(current[0] ? current : "♪", 18, 3, original_lines);
    int translated_count = wrap_lyric(translation, 18, 2, translated_lines);
    const int height = lcd_text_height(18), line_gap = 4, pair_gap = 12;
    int original_height = original_count * height + (original_count - 1) * line_gap;
    int translated_height = translated_count * height + (translated_count - 1) * line_gap;
    int y = 26 + (168 - original_height - pair_gap - translated_height) / 2;
    for (int i = 0; i < original_count; i++) {
        lcd_text_ellipsis(263, y + i * (height + line_gap), 184,
                          original_lines[i], 18, C_TEAL);
    }
    y += original_height + pair_gap;
    for (int i = 0; i < translated_count; i++) {
        lcd_text_ellipsis(263, y + i * (height + line_gap), 184,
                          translated_lines[i], 18, C_MUTED);
    }
}

static int playback_mode(const app_settings_t *cfg)
{
    return cfg->shuffle ? 2 : (cfg->repeat == 2 ? 1 : 0);
}

static void draw_playback_mode_icon(int mode)
{
    const uint8_t *icon = mode_icons_bin_start + mode * 24 * 3;
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) {
            if (icon[y * 3 + x / 8] & (0x80 >> (x % 8)))
                lcd_rect(261 + x, 265 + y, 1, 1, C_TEAL);
        }
    }
}

static void cycle_playback_mode(void)
{
    app_settings_t cfg = *app_settings_get();
    int next = (playback_mode(&cfg) + 1) % 3;
    cfg.shuffle = next == 2;
    cfg.repeat = next == 1 ? 2 : 1;
    app_settings_update(&cfg);
    s_dirty = true;
}

static void refresh_track_lyrics(const music_item_t *item)
{
    const char *path = item ? item->path : "";
    if (strcmp(path, s_track_path) != 0) {
        s_lyric_scrolling = false;
        snprintf(s_track_path, sizeof(s_track_path), "%s", path);
        lcd_lyrics_load(NULL);
        lcd_cover_load(NULL);
        s_track_assets_pending = path[0] != '\0';
        s_scroll_origin_us = esp_timer_get_time();
        s_last_lyric_current = NULL;
    }
}

static void draw_home_title(const music_item_t *item)
{
    lcd_rect(HOME_TITLE_X, HOME_TITLE_Y, HOME_TITLE_W, lcd_text_height(24), C_BG);
    lcd_text_marquee(HOME_TITLE_X, HOME_TITLE_Y, HOME_TITLE_W,
                     item && item->title[0] ? item->title : "暂无曲目",
                     24, C_INK,
                     (uint32_t)((esp_timer_get_time() - s_scroll_origin_us) /
                                HOME_TITLE_FRAME_US));
}

static bool home_lyrics_changed(uint32_t pos)
{
    const char *previous, *current, *translation, *next;
    lcd_lyrics_at(pos, &previous, &current, &translation, &next);
    return previous != s_last_lyric_previous || current != s_last_lyric_current ||
           translation != s_last_lyric_translation || next != s_last_lyric_next;
}

static void draw_home_lyrics(uint32_t pos)
{
    lcd_rect(LYRIC_X, LYRIC_Y, LYRIC_W, LYRIC_H, C_CARD);
    if (lcd_lyrics_available()) {
        const char *previous, *current, *translation, *next;
        lcd_lyrics_at(pos, &previous, &current, &translation, &next);
        s_last_lyric_previous = previous;
        s_last_lyric_current = current;
        s_last_lyric_translation = translation;
        s_last_lyric_next = next;
        if (translation[0]) {
            lcd_round_rect(256, 26, 198, 168, 8, C_SOFT);
            draw_bilingual_lyric(current, translation);
        } else {
            lcd_text_ellipsis(261, 26, 188, previous, 18, 0x97A5A3);
            lcd_round_rect(256, 59, 198, 104, 8, C_SOFT);
            draw_active_lyric(current);
            lcd_text_ellipsis(261, 171, 188, next, 18, 0x97A5A3);
        }
    } else {
        s_last_lyric_previous = NULL;
        s_last_lyric_current = NULL;
        s_last_lyric_translation = NULL;
        s_last_lyric_next = NULL;
        lcd_text_center(260, 75, 190, "暂无歌词", 24, C_MUTED);
        lcd_text_center(259, 118, 192, "同名 .lrc 可同步显示", 18, 0x97A5A3);
    }
}

static void copy_lyric_pixels(uint16_t *pixels)
{
    for (int y = 0; y < LYRIC_H; y++) {
        memcpy(pixels + (size_t)y * LYRIC_W,
               s_frame + (size_t)(LYRIC_Y + y) * LCD_W + LYRIC_X,
               LYRIC_W * sizeof(uint16_t));
    }
}

static void start_lyric_scroll(uint32_t pos)
{
    const size_t pixels = (size_t)LYRIC_W * LYRIC_H;
    copy_lyric_pixels(s_lyric_frames);
    draw_home_lyrics(pos);
    copy_lyric_pixels(s_lyric_frames + pixels);
    for (int y = 0; y < LYRIC_H; y++) {
        memcpy(s_frame + (size_t)(LYRIC_Y + y) * LCD_W + LYRIC_X,
               s_lyric_frames + (size_t)y * LYRIC_W,
               LYRIC_W * sizeof(uint16_t));
    }
    s_lyric_scroll_start_us = esp_timer_get_time();
    s_last_lyric_frame_us = s_lyric_scroll_start_us;
    s_lyric_scrolling = true;
}

static uint16_t blend_lyric_pixel(uint16_t old, uint16_t next, unsigned alpha)
{
    unsigned inv = 32 - alpha;
    unsigned r = (((old >> 11) * inv + (next >> 11) * alpha) + 16) >> 5;
    unsigned g = ((((old >> 5) & 63) * inv + ((next >> 5) & 63) * alpha) + 16) >> 5;
    unsigned b = (((old & 31) * inv + (next & 31) * alpha) + 16) >> 5;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool draw_lyric_scroll_frame(int64_t now)
{
    const size_t pixels = (size_t)LYRIC_W * LYRIC_H;
    const uint16_t *old = s_lyric_frames;
    const uint16_t *next = s_lyric_frames + pixels;
    int64_t elapsed = now - s_lyric_scroll_start_us;
    unsigned phase = elapsed >= LYRIC_SCROLL_US ? 256 :
                     elapsed <= 0 ? 0 : (unsigned)(elapsed * 256 / LYRIC_SCROLL_US);
    unsigned eased = phase * phase * (768 - 2 * phase) / (256 * 256);
    unsigned alpha = (eased * 32 + 128) / 256;
    int old_shift = (LYRIC_SCROLL_PX * (int)eased + 128) / 256;
    int new_shift = (LYRIC_SCROLL_PX * (256 - (int)eased) + 128) / 256;
    for (int y = 0; y < LYRIC_H; y++) {
        uint16_t *dest = s_frame + (size_t)(LYRIC_Y + y) * LCD_W + LYRIC_X;
        const uint16_t *old_row = y + old_shift < LYRIC_H ?
                                  old + (size_t)(y + old_shift) * LYRIC_W : NULL;
        const uint16_t *new_row = y >= new_shift ?
                                  next + (size_t)(y - new_shift) * LYRIC_W : NULL;
        if (alpha == 0 && old_row) {
            memcpy(dest, old_row, LYRIC_W * sizeof(uint16_t));
        } else if (alpha == 32 && new_row) {
            memcpy(dest, new_row, LYRIC_W * sizeof(uint16_t));
        } else {
            for (int x = 0; x < LYRIC_W; x++) {
                dest[x] = blend_lyric_pixel(old_row ? old_row[x] :
                                                old[(size_t)(LYRIC_H - 1) * LYRIC_W + x],
                                            new_row ? new_row[x] : next[x], alpha);
            }
        }
    }
    if (lcd_panel_flush_rect(s_frame, LYRIC_X, LYRIC_Y, LYRIC_W, LYRIC_H) != ESP_OK)
        return false;
    s_last_lyric_frame_us = now;
    if (phase == 256) s_lyric_scrolling = false;
    return true;
}

static void draw_home_progress(const music_item_t *item, uint32_t pos, uint32_t dur)
{
    if (dur && pos > dur) pos = dur;
    lcd_rect(261, 226, 188, HOME_PROGRESS_H, C_CARD);
    lcd_round_rect(261, 226, 188, 6, 3, C_BORDER);
    if (dur) {
        int filled = (int)((uint64_t)pos * 188 / dur);
        if (filled > 0) lcd_round_rect(261, 226, filled, 6, 3, C_TEAL);
    }
    char elapsed[16], total[16];
    time_text(elapsed, sizeof(elapsed), pos);
    if (dur) time_text(total, sizeof(total), dur);
    else snprintf(total, sizeof(total), "--:--");
    int total_x = 449 - lcd_text_width(total, 18);
    lcd_text(261, 234, elapsed, 18, C_MUTED);
    const char *status = player_get_state() == PLAYER_ERROR ? "播放失败" :
                         item && !player_can_seek() ? "不可快进" : NULL;
    if (status) {
        int gap_x = 261 + lcd_text_width(elapsed, 18);
        int gap_w = total_x - gap_x;
        int status_w = lcd_text_width(status, 18);
        if (gap_w >= status_w + 8)
            lcd_text(gap_x + (gap_w - status_w) / 2, 234, status, 18, C_MUTED);
    }
    lcd_text(total_x, 234, total, 18, C_MUTED);
}

static void draw_home(void)
{
    s_lyric_scrolling = false;
    const music_item_t *item = player_get_current();
    refresh_track_lyrics(item);
    const player_state_t state = player_get_state();
    uint32_t pos = player_get_pos_ms();
    uint32_t dur = player_get_dur_ms();
    if (s_touch_down && s_touch_capture == TOUCH_CAPTURE_SEEK)
        pos = s_seek_preview_ms;
    if (dur && pos > dur) pos = dur;

    lcd_gfx_bind(s_frame);
    lcd_clear(C_BG);
    draw_cover();
    draw_home_title(item);
    lcd_text_ellipsis(18, 272, 177, item && item->artist[0] ? item->artist :
                      (music_lib_scanning() ? "正在扫描曲库" : "未知艺术家"), 18, C_MUTED);
    draw_library_icon();

    /* The whole upper card is reserved for lyrics. */
    lcd_round_rect(246, 16, 218, 188, 16, C_CARD);
    draw_home_lyrics(pos);
    s_last_lyric_pos = pos;

    /* Progress, mode and transport controls retain 44+ px touch targets. */
    lcd_round_rect(246, 214, 218, 90, 16, C_CARD);
    draw_home_progress(item, pos, dur);

    draw_playback_mode_icon(playback_mode(app_settings_get()));
    draw_prev_icon(319, 277, C_INK);
    lcd_circle(371, 277, 22, C_TEAL);
    if (state == PLAYER_PLAYING) {
        lcd_rect(363, 267, 5, 20, C_CARD);
        lcd_rect(374, 267, 5, 20, C_CARD);
    } else {
        lcd_triangle(365, 266, 365, 288, 383, 277, C_CARD);
    }
    draw_next_icon(426, 277, C_INK);
}

static void draw_library_cover(int slot, const char *path, int x, int y)
{
    const uint16_t *art = lcd_cover_thumb_pixels(slot, path);
    if (art) {
        for (int dy = 0; dy < LCD_COVER_THUMB_SIZE; dy++) {
            for (int dx = 0; dx < LCD_COVER_THUMB_SIZE; dx++) {
                int corner_x = dx < 5 ? 4 - dx : dx > 42 ? dx - 43 : 0;
                int corner_y = dy < 5 ? 4 - dy : dy > 42 ? dy - 43 : 0;
                if (corner_x * corner_x + corner_y * corner_y <= 16)
                    s_frame[(y + dy) * LCD_W + x + dx] = art[dy * LCD_COVER_THUMB_SIZE + dx];
            }
        }
        return;
    }
    lcd_round_rect(x, y, 48, 48, 5, C_COVER_A);
    lcd_circle(x + 24, y + 24, 17, C_COVER_B);
    lcd_circle(x + 24, y + 24, 5, C_CARD);
}

static void music_notice(const char *message)
{
    snprintf(s_music_notice, sizeof(s_music_notice), "%s", message);
    s_music_notice_until_us = esp_timer_get_time() + 1800000;
    s_dirty = true;
}

static void draw_music_notice(void)
{
    if (s_music_notice[0] && esp_timer_get_time() < s_music_notice_until_us) {
        lcd_round_rect(118, 265, 244, 38, 5, C_INK);
        lcd_text_center(118, 271, 244, s_music_notice, 18, C_CARD);
    }
}

static void draw_favorite_icon(int cx, int cy, bool filled)
{
    static const int8_t points[10][2] = {
        {0, -12}, {3, -4}, {12, -4}, {5, 2}, {8, 11},
        {0, 6}, {-8, 11}, {-5, 2}, {-12, -4}, {-3, -4},
    };
    uint32_t color = filled ? C_TEAL : C_MUTED;
    for (int i = 0; i < 10; i++) {
        int next = (i + 1) % 10;
        if (filled)
            lcd_triangle(cx, cy, cx + points[i][0], cy + points[i][1],
                         cx + points[next][0], cy + points[next][1], color);
        else
            lcd_line(cx + points[i][0], cy + points[i][1],
                     cx + points[next][0], cy + points[next][1], 2, color);
    }
}

static bool reserve_indices(int **indices, int *capacity, int needed)
{
    if (*capacity >= needed) return true;
    int *replacement = heap_caps_malloc((size_t)needed * sizeof(int),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!replacement) replacement = malloc((size_t)needed * sizeof(int));
    if (!replacement) return false;
    free(*indices);
    *indices = replacement;
    *capacity = needed;
    return true;
}

static bool artist_separator(const char *p, const char *start, int *length)
{
    if (*p == ',' || *p == ';' || *p == '/') { *length = 1; return true; }
    if (*p == '&' && isspace((unsigned char)p[1]) &&
        (p == start || isspace((unsigned char)p[-1]))) {
        *length = 1;
        return true;
    }
    if (strncmp(p, "，", 3) == 0 || strncmp(p, "、", 3) == 0) {
        *length = 3;
        return true;
    }
    return false;
}

static bool next_artist(const char **cursor, char out[64])
{
    const char *p = *cursor;
    int separator_len = 0;
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (artist_separator(p, *cursor, &separator_len)) {
            p += separator_len;
            continue;
        }
        break;
    }
    if (!*p) { *cursor = p; return false; }
    const char *start = p;
    while (*p && !artist_separator(p, start, &separator_len)) p++;
    const char *end = p;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t length = (size_t)(end - start);
    if (length >= 64) length = 63;
    memcpy(out, start, length);
    out[length] = '\0';
    *cursor = p;
    return length > 0;
}

static void category_key(const music_item_t *item, category_mode_t mode,
                         char out[MUSIC_PATH_MAX])
{
    out[0] = '\0';
    if (!item) return;
    switch (mode) {
    case CATEGORY_TITLE: {
        const unsigned char *p = (const unsigned char *)item->title;
        while (*p && isspace(*p)) p++;
        if (!p[0]) { snprintf(out, MUSIC_PATH_MAX, "其他"); break; }
        if (p[0] >= '0' && p[0] <= '9') {
            snprintf(out, MUSIC_PATH_MAX, "0-9");
        } else if (isalpha(p[0])) {
            out[0] = (char)toupper(p[0]);
            out[1] = '\0';
        } else if (p[0] < 0x80) {
            snprintf(out, MUSIC_PATH_MAX, "其他");
        } else {
            int length = (p[0] & 0xE0) == 0xC0 ? 2 :
                         (p[0] & 0xF0) == 0xE0 ? 3 : 4;
            memcpy(out, p, (size_t)length);
            out[length] = '\0';
        }
        break;
    }
    case CATEGORY_ARTIST:
        snprintf(out, MUSIC_PATH_MAX, "%s", item->artist[0] ? item->artist : "未知歌手");
        break;
    case CATEGORY_ALBUM:
        snprintf(out, MUSIC_PATH_MAX, "%s", item->album[0] ? item->album : "未知专辑");
        break;
    case CATEGORY_PUBLISHER:
        snprintf(out, MUSIC_PATH_MAX, "%s", item->publisher[0] ?
                 item->publisher : "未知发行方");
        break;
    case CATEGORY_FOLDER: {
        snprintf(out, MUSIC_PATH_MAX, "%s", item->path);
        char *slash = strrchr(out, '/');
        if (slash) *slash = '\0';
        if (strncmp(out, MUSIC_SCAN_DIR, strlen(MUSIC_SCAN_DIR)) == 0 &&
            (out[strlen(MUSIC_SCAN_DIR)] == '/' || !out[strlen(MUSIC_SCAN_DIR)])) {
            memmove(out, out + strlen(MUSIC_SCAN_DIR),
                    strlen(out + strlen(MUSIC_SCAN_DIR)) + 1);
            if (out[0] == '/') memmove(out, out + 1, strlen(out));
        } else if (strncmp(out, "/sdcard/", 8) == 0) {
            memmove(out, out + 8, strlen(out + 8) + 1);
        }
        if (!out[0]) snprintf(out, MUSIC_PATH_MAX, "根目录");
        break;
    }
    }
}

static bool category_matches(const music_item_t *item, const char *key)
{
    if (s_category_mode == CATEGORY_ARTIST && item->artist[0]) {
        const char *cursor = item->artist;
        char token[64];
        while (next_artist(&cursor, token)) {
            if (strcasecmp(token, key) == 0) return true;
        }
        return false;
    }
    char candidate[MUSIC_PATH_MAX];
    category_key(item, s_category_mode, candidate);
    return strcasecmp(candidate, key) == 0;
}

static bool category_add_group(const char *key, int item_index)
{
    int low = 0, high = s_category_group_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int cmp = strcasecmp(s_category_groups[mid].key, key);
        if (cmp < 0) low = mid + 1;
        else high = mid;
    }
    if (low < s_category_group_count &&
        strcasecmp(s_category_groups[low].key, key) == 0) {
        if (s_category_groups[low].last_index != item_index) {
            s_category_groups[low].count++;
            s_category_groups[low].last_index = item_index;
        }
        return true;
    }
    if (s_category_group_count == s_category_group_capacity) {
        int next = s_category_group_capacity ? s_category_group_capacity * 2 : 16;
        category_group_t *replacement = heap_caps_calloc((size_t)next,
            sizeof(category_group_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!replacement) replacement = calloc((size_t)next, sizeof(category_group_t));
        if (!replacement) return false;
        if (s_category_groups)
            memcpy(replacement, s_category_groups,
                   (size_t)s_category_group_count * sizeof(category_group_t));
        free(s_category_groups);
        s_category_groups = replacement;
        s_category_group_capacity = next;
    }
    size_t length = strlen(key) + 1;
    char *copy = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = malloc(length);
    if (!copy) return false;
    memcpy(copy, key, length);
    if (low < s_category_group_count)
        memmove(&s_category_groups[low + 1], &s_category_groups[low],
                (size_t)(s_category_group_count - low) * sizeof(category_group_t));
    category_group_t *group = &s_category_groups[low];
    s_category_group_count++;
    group->key = copy;
    group->count = 1;
    group->last_index = item_index;
    return true;
}

static void category_build_tracks(void)
{
    s_category_track_count = 0;
    if (!s_category_in_group || music_lib_scanning()) return;
    int count = music_lib_count();
    if (!reserve_indices(&s_category_tracks, &s_category_track_capacity, count)) {
        music_notice("曲库内存不足");
        return;
    }
    for (int i = 0; i < count; i++) {
        const music_item_t *item = music_lib_get(i);
        if (item && category_matches(item, s_category_key))
            s_category_tracks[s_category_track_count++] = i;
    }
    if (s_category_track_selected >= s_category_track_count)
        s_category_track_selected = s_category_track_count - 1;
    if (s_category_track_selected < 0) s_category_track_selected = -1;
    int max_top = s_category_track_count > LIB_ROWS ?
                  s_category_track_count - LIB_ROWS : 0;
    if (s_category_track_top > max_top) s_category_track_top = max_top;
}

static void category_rebuild(void)
{
    for (int i = 0; i < s_category_group_count; i++) free(s_category_groups[i].key);
    s_category_group_count = 0;
    s_category_top = 0;
    s_category_selected = -1;
    if (music_lib_scanning()) return;
    int count = music_lib_count();
    for (int i = 0; i < count; i++) {
        const music_item_t *item = music_lib_get(i);
        if (!item) continue;
        if (s_category_mode == CATEGORY_ARTIST && item->artist[0]) {
            const char *cursor = item->artist;
            char token[64];
            while (next_artist(&cursor, token)) {
                if (!category_add_group(token, i)) { music_notice("分类内存不足"); break; }
            }
        } else {
            char key[MUSIC_PATH_MAX];
            category_key(item, s_category_mode, key);
            if (!category_add_group(key, i)) { music_notice("分类内存不足"); break; }
        }
    }
    category_build_tracks();
}

static void favorites_rebuild(void)
{
    s_favorite_count = 0;
    if (music_lib_scanning()) return;
    int count = music_favorites_count();
    if (!reserve_indices(&s_favorite_tracks, &s_favorite_capacity, count)) {
        music_notice("收藏内存不足");
        return;
    }
    for (int i = 0; i < count; i++) {
        int index = music_favorites_index_at(i);
        if (index >= 0 && music_lib_get(index))
            s_favorite_tracks[s_favorite_count++] = index;
    }
    if (s_favorite_selected >= s_favorite_count)
        s_favorite_selected = s_favorite_count - 1;
    int max_top = s_favorite_count > LIB_ROWS ? s_favorite_count - LIB_ROWS : 0;
    if (s_favorite_top > max_top) s_favorite_top = max_top;
}

static void toggle_favorite(int index)
{
    const music_item_t *item = music_lib_get(index);
    if (!item) return;
    bool active = false;
    if (music_favorites_toggle(item->path, &active) != ESP_OK) {
        music_notice("收藏保存失败");
        return;
    }
    if (s_page == PAGE_FAVORITES) favorites_rebuild();
    s_dirty = true;
}

static void collection_move(int *selected, int *top, int count, int rows,
                            int step)
{
    int total = count + 1;
    *selected = (*selected + 1 + step + total) % total - 1;
    if (*selected < 0) *top = 0;
    else if (*selected < *top) *top = *selected;
    else if (*selected >= *top + rows) *top = *selected - rows + 1;
    s_dirty = true;
}

static void collection_scroll(int *selected, int *top, int count, int rows,
                              int delta)
{
    int max_top = count > rows ? count - rows : 0;
    int next = *top + delta;
    if (next < 0) next = 0;
    if (next > max_top) next = max_top;
    if (next == *top) return;
    *top = next;
    if (*selected < *top || *selected >= *top + rows) *selected = *top;
    s_last_list_scroll_us = esp_timer_get_time();
    s_dirty = true;
}

static void open_category(void)
{
    s_category_in_group = false;
    category_rebuild();
    s_page = PAGE_CATEGORY;
    s_dirty = true;
}

static void open_favorites(void)
{
    favorites_rebuild();
    s_page = PAGE_FAVORITES;
    s_dirty = true;
}

static void category_back(void)
{
    if (s_category_in_group) {
        s_category_in_group = false;
        s_dirty = true;
    } else open_library();
}

static void category_activate(void)
{
    if (s_category_in_group) {
        if (s_category_track_selected < 0) { category_back(); return; }
        int index = s_category_tracks[s_category_track_selected];
        if (player_start_collection(index, s_category_tracks,
                                    s_category_track_count, true) == ESP_OK) {
            s_page = PAGE_HOME;
            s_dirty = true;
        } else music_notice("播放失败");
        return;
    }
    if (s_category_selected < 0) { category_back(); return; }
    category_group_t *group = &s_category_groups[s_category_selected];
    snprintf(s_category_key, sizeof(s_category_key), "%s", group->key);
    s_category_in_group = true;
    s_category_track_selected = -1;
    s_category_track_top = 0;
    category_build_tracks();
    s_dirty = true;
}

static void favorites_activate(void)
{
    if (s_favorite_selected < 0) { open_category(); return; }
    int index = s_favorite_tracks[s_favorite_selected];
    if (player_start_collection(index, s_favorite_tracks,
                                s_favorite_count, true) == ESP_OK) {
        s_page = PAGE_HOME;
        s_dirty = true;
    } else music_notice("播放失败");
}

static void library_clamp_selection(void)
{
    int count = music_lib_scanning() ? 0 : music_lib_count();
    if (count <= 0) {
        s_lib_selected = -1;
        s_lib_top = 0;
    } else {
        if (s_lib_selected >= count) s_lib_selected = count - 1;
        if (s_lib_selected < -1) s_lib_selected = -1;
        if (s_lib_selected < 0) s_lib_top = 0;
        else if (s_lib_selected < s_lib_top) s_lib_top = s_lib_selected;
        else if (s_lib_selected >= s_lib_top + LIB_ROWS)
            s_lib_top = s_lib_selected - LIB_ROWS + 1;
        int max_top = count > LIB_ROWS ? count - LIB_ROWS : 0;
        if (s_lib_top > max_top) s_lib_top = max_top;
    }
}

static void sync_library_generation(void)
{
    if (music_lib_scanning()) return;
    uint32_t generation = music_lib_generation();
    if (generation == s_library_generation) return;
    lcd_cover_thumb_clear();
    s_track_path[0] = '\0';
    library_clamp_selection();
    category_rebuild();
    favorites_rebuild();
    s_library_generation = generation;
    s_dirty = true;
}

static void open_library(void)
{
    const music_item_t *playing = player_get_current();
    if (!music_lib_scanning() && playing) {
        int count = music_lib_count();
        for (int i = 0; i < count; i++) {
            const music_item_t *item = music_lib_get(i);
            if (item && strcmp(item->path, playing->path) == 0) {
                s_lib_selected = i;
                break;
            }
        }
    }
    library_clamp_selection();
    s_page = PAGE_LIBRARY;
    s_dirty = true;
}

static void library_move(int step)
{
    if (music_lib_scanning()) return;
    int count = music_lib_count();
    if (count <= 0) return;
    int total = count + 1;
    s_lib_selected = (s_lib_selected + 1 + step + total) % total - 1;
    library_clamp_selection();
    s_dirty = true;
}

static void library_scroll(int delta)
{
    if (music_lib_scanning()) return;
    int count = music_lib_count();
    int max_top = count > LIB_ROWS ? count - LIB_ROWS : 0;
    int next = s_lib_top + delta;
    if (next < 0) next = 0;
    if (next > max_top) next = max_top;
    if (next == s_lib_top) return;
    s_lib_top = next;
    if (s_lib_selected < s_lib_top || s_lib_selected >= s_lib_top + LIB_ROWS)
        s_lib_selected = s_lib_top;
    s_last_list_scroll_us = esp_timer_get_time();
    s_dirty = true;
}

static void library_play(int index)
{
    if (index == -1) { s_page = PAGE_HOME; s_dirty = true; return; }
    if (music_lib_scanning() || index < 0 || index >= music_lib_count()) return;
    s_lib_selected = index;
    if (player_start_index(index, true) == ESP_OK) {
        s_page = PAGE_HOME;
        s_dirty = true;
    }
}

static void draw_music_header(const char *title, const char *first,
                              const char *second, int count, const char *unit,
                              bool back_selected)
{
    if (back_selected) lcd_round_rect(8, 7, 48, 42, 6, C_SOFT);
    lcd_line(39, 30, 20, 30, 2, C_INK);
    lcd_line(20, 30, 29, 21, 2, C_INK);
    lcd_line(20, 30, 29, 39, 2, C_INK);
    lcd_text(60, 14, title, 24, C_INK);
    lcd_text_center(128, 18, 58, first, 18, C_TEAL);
    lcd_text_center(194, 18, 62, second, 18, C_TEAL);
    draw_bt_icon(286, 30, C_TEAL);
    char total[16];
    snprintf(total, sizeof(total), "%d %s", count, unit);
    lcd_text_center(355, 18, 109, total, 18, C_MUTED);
    lcd_rect(16, 51, 448, 1, C_BORDER);
}

static void draw_group_header(void)
{
    bool back_selected = s_category_track_selected == -1;
    if (back_selected) lcd_round_rect(8, 7, 48, 42, 6, C_SOFT);
    lcd_line(39, 30, 20, 30, 2, C_INK);
    lcd_line(20, 30, 29, 21, 2, C_INK);
    lcd_line(20, 30, 29, 39, 2, C_INK);
    lcd_text_ellipsis(60, 14, 295, s_category_key, 24, C_INK);
    char total[16];
    snprintf(total, sizeof(total), "%d 首", s_category_track_count);
    lcd_text_center(365, 18, 99, total, 18, C_MUTED);
    lcd_rect(16, 51, 448, 1, C_BORDER);
}

static void draw_scrollbar(int top, int visible, int count, int y, int height)
{
    if (count <= visible) return;
    int thumb_height = height * visible / count;
    if (thumb_height < 22) thumb_height = 22;
    int travel = height - thumb_height;
    int thumb_y = y + travel * top / (count - visible);
    lcd_round_rect(473, y, 3, height, 1, C_BORDER);
    lcd_round_rect(473, thumb_y, 3, thumb_height, 1, C_TEAL);
}

static void draw_song_row(int slot, int index, int y, bool selected)
{
    const music_item_t *item = music_lib_get(index);
    if (!item) return;
    if (selected) lcd_round_rect(12, y, 456, 60, 6, C_SOFT);
    else lcd_rect(84, y + LIB_ROW_H - 1, 380, 1, C_BORDER);
    draw_library_cover(slot, item->path, 20, y + 6);
    lcd_text_ellipsis(84, y + 3, 314, item->title[0] ? item->title : "未知曲目",
                      24, C_INK);
    lcd_text_ellipsis(84, y + 33, 314,
                      item->artist[0] ? item->artist : "未知艺术家", 18, C_MUTED);
    if (player_get_current() &&
        strcmp(player_get_current()->path, item->path) == 0)
        lcd_rect(406, y + 23, 3, 16, C_TEAL);
    draw_favorite_icon(440, y + 31,
                       music_favorites_contains(item->path));
}

static void draw_library(void)
{
    lcd_gfx_bind(s_frame);
    lcd_clear(C_BG);
    bool scanning = music_lib_scanning();
    int count = scanning ? 0 : music_lib_count();
    draw_music_header("曲库", "分类", "收藏", count, "首",
                      s_lib_selected == -1);

    if (scanning || count <= 0) {
        lcd_text_center(16, 139, 448, scanning ? "正在扫描曲库" : "暂无曲目",
                        24, C_MUTED);
        return;
    }
    for (int row = 0; row < LIB_ROWS; row++) {
        int index = s_lib_top + row;
        if (index >= count) break;
        int y = LIB_ROW_Y + row * LIB_ROW_H;
        draw_song_row(row, index, y, index == s_lib_selected);
    }
    draw_scrollbar(s_lib_top, LIB_ROWS, count, LIB_ROW_Y, LIB_ROWS * LIB_ROW_H);
    draw_music_notice();
}

static void draw_category(void)
{
    static const char *tabs[CATEGORY_MODES] = {
        "歌名", "歌手", "专辑", "发行", "目录",
    };
    lcd_gfx_bind(s_frame);
    lcd_clear(C_BG);
    bool scanning = music_lib_scanning();
    if (s_category_in_group) {
        draw_group_header();
        if (scanning || s_category_track_count == 0) {
            lcd_text_center(16, 139, 448,
                            scanning ? "正在扫描曲库" : "此合集暂无曲目",
                            24, C_MUTED);
        } else {
            for (int row = 0; row < LIB_ROWS; row++) {
                int position = s_category_track_top + row;
                if (position >= s_category_track_count) break;
                draw_song_row(row, s_category_tracks[position],
                              LIB_ROW_Y + row * LIB_ROW_H,
                              position == s_category_track_selected);
            }
            draw_scrollbar(s_category_track_top, LIB_ROWS, s_category_track_count,
                           LIB_ROW_Y, LIB_ROWS * LIB_ROW_H);
        }
        draw_music_notice();
        return;
    }

    draw_music_header("分类", "曲库", "收藏", s_category_group_count, "组",
                      s_category_selected == -1);
    for (int i = 0; i < CATEGORY_MODES; i++) {
        int x = 16 + i * 90;
        bool active = s_category_mode == i;
        lcd_round_rect(x, 58, 82, 39, 5, active ? C_TEAL : C_CARD);
        lcd_text_center(x, 65, 82, tabs[i], 18, active ? C_CARD : C_INK);
    }
    if (scanning || s_category_group_count == 0) {
        lcd_text_center(16, 175, 448,
                        scanning ? "正在扫描曲库" : "暂无分类",
                        24, C_MUTED);
        draw_music_notice();
        return;
    }
    for (int row = 0; row < CATEGORY_ROWS; row++) {
        int position = s_category_top + row;
        if (position >= s_category_group_count) break;
        int y = CATEGORY_ROW_Y + row * CATEGORY_ROW_H;
        const category_group_t *group = &s_category_groups[position];
        if (position == s_category_selected)
            lcd_round_rect(12, y + 2, 456, 49, 6, C_SOFT);
        else lcd_rect(24, y + CATEGORY_ROW_H - 1, 440, 1, C_BORDER);
        lcd_text_ellipsis(24, y + 10, 350, group->key, 24, C_INK);
        char count[16];
        snprintf(count, sizeof(count), "%d 首", group->count);
        lcd_text_ellipsis(383, y + 15, 60, count, 18, C_MUTED);
        lcd_line(449, y + 18, 457, y + 26, 2, C_MUTED);
        lcd_line(457, y + 26, 449, y + 34, 2, C_MUTED);
    }
    draw_scrollbar(s_category_top, CATEGORY_ROWS, s_category_group_count,
                   CATEGORY_ROW_Y, CATEGORY_ROWS * CATEGORY_ROW_H);
    draw_music_notice();
}

static void draw_favorites(void)
{
    lcd_gfx_bind(s_frame);
    lcd_clear(C_BG);
    bool scanning = music_lib_scanning();
    draw_music_header("收藏", "曲库", "分类", s_favorite_count, "首",
                      s_favorite_selected == -1);
    if (scanning || s_favorite_count == 0) {
        lcd_text_center(16, 139, 448,
                        scanning ? "正在扫描曲库" : "暂无收藏曲目",
                        24, C_MUTED);
        draw_music_notice();
        return;
    }
    for (int row = 0; row < LIB_ROWS; row++) {
        int position = s_favorite_top + row;
        if (position >= s_favorite_count) break;
        draw_song_row(row, s_favorite_tracks[position],
                      LIB_ROW_Y + row * LIB_ROW_H,
                      position == s_favorite_selected);
    }
    draw_scrollbar(s_favorite_top, LIB_ROWS, s_favorite_count,
                   LIB_ROW_Y, LIB_ROWS * LIB_ROW_H);
    draw_music_notice();
}

static void bt_notice(const char *message)
{
    snprintf(s_bt_notice, sizeof(s_bt_notice), "%s", message);
    s_bt_notice_until_us = esp_timer_get_time() + 2200000;
    s_dirty = true;
}

static void bt_target(char addr[18], char name[32])
{
    bt_state_t state = bt_audio_state();
    const char *current = bt_audio_current_addr();
    if ((state == BT_ST_CONNECTING || state == BT_ST_CONNECTED ||
         state == BT_ST_STREAMING) && current[0]) {
        snprintf(addr, 18, "%s", current);
        snprintf(name, 32, "%s", bt_audio_connected_name());
    } else {
        const app_settings_t *settings = app_settings_get();
        snprintf(addr, 18, "%s", settings->last_peer_addr);
        snprintf(name, 32, "%s", settings->last_peer_name);
    }
    if (!name[0] || strcmp(name, addr) == 0)
        snprintf(name, 32, "%s", "蓝牙耳机");
}

static void bt_clamp_selection(void)
{
    int count = bt_audio_peer_count();
    int total = 3 + count;
    if (s_bt_selected < 0) s_bt_selected = 0;
    if (s_bt_selected >= total) s_bt_selected = total - 1;
    if (s_bt_selected >= 3) {
        int peer = s_bt_selected - 3;
        if (peer < s_bt_top) s_bt_top = peer;
        else if (peer >= s_bt_top + BT_ROWS) s_bt_top = peer - BT_ROWS + 1;
    }
    int max_top = count > BT_ROWS ? count - BT_ROWS : 0;
    if (s_bt_top > max_top) s_bt_top = max_top;
    if (s_bt_top < 0) s_bt_top = 0;
}

static void open_bt(void)
{
    s_page = PAGE_BT;
    bt_state_t state = bt_audio_state();
    s_bt_selected = state == BT_ST_CONNECTING || state == BT_ST_CONNECTED ||
                    state == BT_ST_STREAMING ? 1 : bt_audio_peer_count() ? 3 : 0;
    s_bt_top = 0;
    s_bt_confirm_forget = false;
    s_bt_cancel_sent = false;
    s_bt_notice[0] = '\0';
    s_bt_last_forget_status = bt_audio_forget_status();
    s_bt_scan_pending = bt_audio_peer_count() == 0 &&
                        state != BT_ST_CONNECTING && state != BT_ST_CONNECTED &&
                        state != BT_ST_STREAMING && state != BT_ST_DISCOVERING;
    s_dirty = true;
}

static void bt_leave(ui_page_t next)
{
    if (bt_audio_state() == BT_ST_DISCOVERING) bt_audio_stop_discovery();
    s_bt_scan_pending = false;
    s_bt_confirm_forget = false;
    if (next == PAGE_LIBRARY) open_library();
    else if (next == PAGE_FAVORITES) open_favorites();
    else { s_page = next; s_dirty = true; }
}

static void bt_confirm_forget(const char *addr, const char *name)
{
    if (!addr || !addr[0] || bt_audio_forget_status() == BT_FORGET_PENDING) return;
    snprintf(s_bt_forget_addr, sizeof(s_bt_forget_addr), "%s", addr);
    snprintf(s_bt_forget_name, sizeof(s_bt_forget_name), "%s", name && name[0] ? name : addr);
    s_bt_confirm_forget = true;
    s_bt_confirm_yes = false;
    s_dirty = true;
}

static void bt_finish_forget(void)
{
    s_bt_confirm_forget = false;
    if (bt_audio_forget_addr(s_bt_forget_addr) == ESP_OK) bt_notice("正在忘记设备");
    else bt_notice("无法忘记设备");
}

static void bt_search_action(void)
{
    bt_state_t state = bt_audio_state();
    if (state == BT_ST_DISCOVERING) {
        bt_audio_stop_discovery();
    } else if (state == BT_ST_READY) {
        s_bt_top = 0;
        if (bt_audio_discovering()) s_bt_scan_pending = true;
        else if (bt_audio_start_discovery(0) != ESP_OK) bt_notice("搜索启动失败");
    } else {
        bt_notice("断开连接后搜索");
    }
    s_dirty = true;
}

static void bt_primary_action(void)
{
    char addr[18], name[32];
    bt_target(addr, name);
    bt_state_t state = bt_audio_state();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (state == BT_ST_CONNECTING) {
        err = bt_audio_cancel_connect();
        if (err == ESP_OK) {
            s_bt_cancel_sent = true;
            bt_notice("正在取消连接");
        }
    } else if (state == BT_ST_CONNECTED || state == BT_ST_STREAMING) {
        err = bt_audio_disconnect();
        if (err == ESP_OK) bt_notice("正在断开连接");
    } else if (addr[0] && (state == BT_ST_READY || state == BT_ST_DISCOVERING)) {
        err = bt_audio_connect_addr(addr);
    }
    if (err != ESP_OK) bt_notice("操作未完成");
    s_dirty = true;
}

static void bt_peer_action(int index)
{
    if (index < 0 || index >= bt_audio_peer_count()) return;
    bt_state_t state = bt_audio_state();
    if (state != BT_ST_READY && state != BT_ST_DISCOVERING) {
        bt_notice("请先断开当前设备");
        return;
    }
    if (bt_audio_connect(index) != ESP_OK) bt_notice("连接启动失败");
    s_dirty = true;
}

static void bt_move(int step)
{
    int total = 3 + bt_audio_peer_count();
    s_bt_selected = (s_bt_selected + step + total) % total;
    bt_clamp_selection();
    s_dirty = true;
}

static void bt_scroll(int delta)
{
    int count = bt_audio_peer_count();
    int max_top = count > BT_ROWS ? count - BT_ROWS : 0;
    int next = s_bt_top + delta;
    if (next < 0) next = 0;
    if (next > max_top) next = max_top;
    if (next == s_bt_top) return;
    s_bt_top = next;
    s_bt_selected = 3 + s_bt_top;
    s_dirty = true;
}

static void bt_activate(void)
{
    if (s_bt_selected == 0) bt_search_action();
    else if (s_bt_selected == 1) bt_primary_action();
    else if (s_bt_selected == 2) {
        char addr[18], name[32];
        bt_target(addr, name);
        if (bt_audio_state() != BT_ST_CONNECTING) bt_confirm_forget(addr, name);
    } else bt_peer_action(s_bt_selected - 3);
}

static const char *bt_status_text(bt_state_t state)
{
    switch (state) {
    case BT_ST_OFF: return "蓝牙关闭";
    case BT_ST_INITING: return "初始化中";
    case BT_ST_DISCOVERING: return "搜索中";
    case BT_ST_CONNECTING: return "连接中";
    case BT_ST_CONNECTED: return "已连接";
    case BT_ST_STREAMING: return "播放中";
    case BT_ST_ERROR: return "蓝牙故障";
    default: return "未连接";
    }
}

static void draw_bt_button(int x, const char *label, bool enabled,
                           bool selected, bool primary)
{
    if (selected) lcd_round_rect(x - 2, 73, 90, 48, 7, C_INK);
    lcd_round_rect(x, 75, 86, 44, 6,
                   !enabled ? C_BORDER : primary ? C_TEAL : C_CARD);
    if (enabled && !primary) {
        lcd_round_rect(x + 1, 76, 84, 42, 5, C_CARD);
    }
    lcd_text_center(x, 84, 86, label, 18,
                    !enabled ? C_MUTED : primary ? C_CARD : C_WARN);
}

static void draw_bt_search_icon(bool active, bool enabled)
{
    if (s_bt_selected == 0) lcd_round_rect(410, 7, 54, 43, 6, C_SOFT);
    uint32_t color = enabled ? C_TEAL : C_MUTED;
    if (active) {
        for (int i = 0; i < 3; i++)
            lcd_circle(423 + i * 12, 29, i == s_bt_anim_phase ? 4 : 3,
                       i == s_bt_anim_phase ? color : C_BORDER);
    } else {
        lcd_circle(431, 26, 9, color);
        lcd_circle(431, 26, 6, s_bt_selected == 0 ? C_SOFT : C_BG);
        lcd_line(437, 32, 445, 40, 2, color);
    }
}

static void draw_bt_confirm(void)
{
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++)
            lcd_gfx_pixel_alpha(x, y, C_INK, 1);
    }
    lcd_round_rect(74, 76, 332, 176, 8, C_BORDER);
    lcd_round_rect(76, 78, 328, 172, 7, C_CARD);
    lcd_text(96, 94, "忘记设备？", 24, C_INK);
    lcd_text_ellipsis(96, 129, 288, s_bt_forget_name, 18, C_MUTED);
    lcd_text_ellipsis(96, 158, 288, "将删除配对记录及自动回连信息", 18, C_MUTED);
    lcd_round_rect(96, 198, 132, 42, 6, s_bt_confirm_yes ? C_BG : C_SOFT);
    lcd_round_rect(252, 198, 132, 42, 6, s_bt_confirm_yes ? C_WARN : C_BG);
    lcd_text_center(96, 207, 132, "取消", 18, C_INK);
    lcd_text_center(252, 207, 132, "确认忘记", 18,
                    s_bt_confirm_yes ? C_CARD : C_WARN);
}

static void draw_bt(void)
{
    bt_clamp_selection();
    bt_state_t state = bt_audio_state();
    bool linked = state == BT_ST_CONNECTED || state == BT_ST_STREAMING;
    bool connecting = state == BT_ST_CONNECTING;
    bool forgetting = bt_audio_forget_status() == BT_FORGET_PENDING;
    char addr[18], name[32];
    bt_target(addr, name);
    int count = bt_audio_peer_count();

    lcd_gfx_bind(s_frame);
    lcd_clear(C_BG);
    lcd_line(39, 30, 20, 30, 2, C_INK);
    lcd_line(20, 30, 29, 21, 2, C_INK);
    lcd_line(20, 30, 29, 39, 2, C_INK);
    lcd_text(60, 14, "蓝牙设备", 24, C_INK);
    if (state == BT_ST_DISCOVERING) lcd_text(271, 18, "搜索中", 18, C_TEAL);
    draw_settings_icon(381, 29, C_TEAL);
    draw_bt_search_icon(state == BT_ST_DISCOVERING,
                        state == BT_ST_DISCOVERING || state == BT_ST_READY);
    lcd_rect(16, 51, 448, 1, C_BORDER);

    lcd_rect(0, 56, LCD_W, 76, C_SOFT);
    lcd_text_ellipsis(20, 63, 252, addr[0] ? name : "尚未连接耳机", 24, C_INK);
    lcd_text(20, 99, connecting && s_bt_cancel_sent ? "取消中" : bt_status_text(state),
             18, C_MUTED);
    const char *codec = bt_audio_active_codec_name();
    bool sbc = linked && strcmp(codec, "SBC") == 0;
    bool aac = linked && strcmp(codec, "AAC") == 0;
    lcd_round_rect(122, 98, 54, 27, 4, sbc ? C_TEAL : C_CARD);
    lcd_round_rect(182, 98, 54, 27, 4, aac ? C_TEAL : C_CARD);
    lcd_text_center(122, 100, 54, "SBC", 18, sbc ? C_CARD : C_MUTED);
    lcd_text_center(182, 100, 54, "AAC", 18, aac ? C_CARD : C_MUTED);

    const char *primary = connecting ? (s_bt_cancel_sent ? "取消中" : "取消连接") :
                          linked ? "断开连接" : "连接设备";
    draw_bt_button(286, primary, !forgetting && !s_bt_cancel_sent &&
                   (connecting || linked || addr[0]),
                   s_bt_selected == 1, true);
    draw_bt_button(380, forgetting ? "忘记中" : "忘记设备",
                   !forgetting && !connecting && addr[0], s_bt_selected == 2, false);

    if (s_bt_notice[0] && esp_timer_get_time() < s_bt_notice_until_us) {
        lcd_text_ellipsis(20, 134, 305, s_bt_notice, 18, C_WARN);
    } else {
        char label[40];
        snprintf(label, sizeof(label), "附近设备  %d", count);
        lcd_text(20, 134, label, 18, C_MUTED);
    }
    lcd_rect(16, 159, 448, 1, C_BORDER);

    if (count == 0) {
        lcd_text_center(16, 213, 448,
                        state == BT_ST_DISCOVERING ? "正在查找附近设备" : "未找到设备",
                        24, C_MUTED);
    } else {
        for (int row = 0; row < BT_ROWS; row++) {
            int index = s_bt_top + row;
            if (index >= count) break;
            char peer_name[32], peer_addr[18];
            snprintf(peer_name, sizeof(peer_name), "%s", bt_audio_peer_name(index));
            snprintf(peer_addr, sizeof(peer_addr), "%s", bt_audio_peer_addr(index));
            int y = BT_ROW_Y + row * BT_ROW_H;
            if (s_bt_selected == index + 3) lcd_round_rect(12, y + 2, 456, 49, 6, C_SOFT);
            else lcd_rect(60, y + BT_ROW_H - 1, 404, 1, C_BORDER);
            lcd_circle(38, y + 26, 16, C_CARD);
            draw_bt_icon(38, y + 26, C_TEAL);
            lcd_text_ellipsis(64, y + 2, 336, peer_name, 18, C_INK);
            lcd_text(64, y + 27, peer_addr, 18, C_MUTED);
            if (bt_audio_addr_known(peer_addr))
                lcd_text_center(410, y + 15, 54, "忘记", 18, C_WARN);
        }
        draw_scrollbar(s_bt_top, BT_ROWS, count, BT_ROW_Y, BT_ROWS * BT_ROW_H);
    }
    if (s_bt_confirm_forget) draw_bt_confirm();
}

static void handle_touch(uint16_t x, uint16_t y)
{
    if (x >= 468 && y >= 56) return;
    if (s_page == PAGE_SETTINGS) {
        settings_result(lcd_settings_touch(x, y));
    } else if (s_page == PAGE_BT) {
        if (s_bt_confirm_forget) {
            if (y >= 198 && y < 240) {
                if (x >= 96 && x < 228) s_bt_confirm_forget = false;
                else if (x >= 252 && x < 384) bt_finish_forget();
            }
        } else if (y <= 52) {
            if (x < 62) bt_leave(PAGE_FAVORITES);
            else if (x >= 350 && x < 408) open_settings();
            else if (x >= 408) { s_bt_selected = 0; bt_search_action(); }
        } else if (y >= 56 && y < 132) {
            if (x >= 286 && x < 378) { s_bt_selected = 1; bt_primary_action(); }
            else if (x >= 378) {
                s_bt_selected = 2;
                char addr[18], name[32];
                bt_target(addr, name);
                if (bt_audio_state() != BT_ST_CONNECTING) bt_confirm_forget(addr, name);
            }
        } else if (y >= BT_ROW_Y && y < BT_ROW_Y + BT_ROWS * BT_ROW_H) {
            int index = s_bt_top + (y - BT_ROW_Y) / BT_ROW_H;
            if (index < bt_audio_peer_count()) {
                s_bt_selected = index + 3;
                char addr[18], name[32];
                snprintf(addr, sizeof(addr), "%s", bt_audio_peer_addr(index));
                snprintf(name, sizeof(name), "%s", bt_audio_peer_name(index));
                if (x >= 410 && bt_audio_addr_known(addr)) bt_confirm_forget(addr, name);
                else bt_peer_action(index);
            }
        }
    } else if (s_page == PAGE_LIBRARY) {
        if (y <= 52) {
            if (x < 62) s_page = PAGE_HOME;
            else if (x >= 120 && x < 190) open_category();
            else if (x >= 190 && x < 264) open_favorites();
            else if (x >= 264 && x < 312) open_bt();
        } else if (y >= LIB_ROW_Y && y < LIB_ROW_Y + LIB_ROWS * LIB_ROW_H &&
                   !music_lib_scanning()) {
            int index = s_lib_top + (y - LIB_ROW_Y) / LIB_ROW_H;
            if (x >= 414) toggle_favorite(index);
            else library_play(index);
        }
    } else if (s_page == PAGE_CATEGORY) {
        if (s_category_in_group) {
            if (y <= 52) {
                if (x < 62) category_back();
            } else if (y >= LIB_ROW_Y && y < LIB_ROW_Y + LIB_ROWS * LIB_ROW_H &&
                       !music_lib_scanning()) {
                int position = s_category_track_top +
                               (y - LIB_ROW_Y) / LIB_ROW_H;
                if (position < s_category_track_count) {
                    s_category_track_selected = position;
                    if (x >= 414) toggle_favorite(s_category_tracks[position]);
                    else category_activate();
                }
            }
        } else if (y <= 52) {
            if (x < 62 || (x >= 120 && x < 190)) open_library();
            else if (x >= 190 && x < 264) open_favorites();
            else if (x >= 264 && x < 312) open_bt();
        } else if (y >= 56 && y < 100 && x >= 16 && x < 458) {
            int mode = (x - 16) / 90;
            if (mode < CATEGORY_MODES && (x - 16) % 90 < 82 &&
                s_category_mode != mode) {
                s_category_mode = mode;
                category_rebuild();
            }
        } else if (y >= CATEGORY_ROW_Y &&
                   y < CATEGORY_ROW_Y + CATEGORY_ROWS * CATEGORY_ROW_H &&
                   !music_lib_scanning()) {
            int position = s_category_top +
                           (y - CATEGORY_ROW_Y) / CATEGORY_ROW_H;
            if (position < s_category_group_count) {
                s_category_selected = position;
                category_activate();
            }
        }
    } else if (s_page == PAGE_FAVORITES) {
        if (y <= 52) {
            if (x < 62) open_category();
            else if (x >= 120 && x < 190) open_library();
            else if (x >= 190 && x < 264) open_category();
            else if (x >= 264 && x < 312) open_bt();
        } else if (y >= LIB_ROW_Y && y < LIB_ROW_Y + LIB_ROWS * LIB_ROW_H &&
                   !music_lib_scanning()) {
            int position = s_favorite_top + (y - LIB_ROW_Y) / LIB_ROW_H;
            if (position < s_favorite_count) {
                s_favorite_selected = position;
                if (x >= 414) toggle_favorite(s_favorite_tracks[position]);
                else favorites_activate();
            }
        }
    } else if (x >= 194 && x <= 239 && y >= 263) {
        open_library();
    } else if (x >= 257 && x <= 451 && y >= 216 && y <= 249) {
        uint32_t dur = player_get_dur_ms();
        int bar_x = (int)x - 261;
        if (bar_x < 0) bar_x = 0;
        if (bar_x > 188) bar_x = 188;
        if (dur && player_can_seek())
            player_seek_ms((uint32_t)((uint64_t)bar_x * dur / 188));
    } else if (y >= 252 && y <= 303) {
        if (x >= 248 && x <= 296) cycle_playback_mode();
        else if (x >= 298 && x <= 341) player_prev();
        else if (x >= 347 && x <= 395) player_toggle();
        else if (x >= 400 && x <= 450) player_next();
    }
    s_dirty = true;
}

static void handle_key(app_key_t key)
{
    sync_library_generation();
    if (s_page == PAGE_SETTINGS) {
        settings_result(lcd_settings_key(key));
        s_dirty = true;
        return;
    }
    if (s_page == PAGE_BT) {
        if (s_bt_confirm_forget) {
            switch (key) {
            case APP_KEY_KEY0_SINGLE:
            case APP_KEY_KEY0_TRIPLE:
            case APP_KEY_UP:
            case APP_KEY_DOWN: s_bt_confirm_yes = !s_bt_confirm_yes; break;
            case APP_KEY_KEY0_DOUBLE:
            case APP_KEY_ENTER:
            case APP_KEY_PLAY:
                if (s_bt_confirm_yes) bt_finish_forget();
                else s_bt_confirm_forget = false;
                break;
            case APP_KEY_BACK:
            case APP_KEY_KEY0_LONG: s_bt_confirm_forget = false; break;
            default: break;
            }
        } else {
            switch (key) {
            case APP_KEY_KEY0_SINGLE:
            case APP_KEY_DOWN:
            case APP_KEY_ROT_CW: bt_move(1); break;
            case APP_KEY_KEY0_TRIPLE:
            case APP_KEY_UP:
            case APP_KEY_ROT_CCW: bt_move(-1); break;
            case APP_KEY_KEY0_DOUBLE:
            case APP_KEY_ENTER:
            case APP_KEY_PLAY: bt_activate(); break;
            case APP_KEY_PAGE_PREV: bt_leave(PAGE_FAVORITES); break;
            case APP_KEY_KEY0_LONG:
            case APP_KEY_BACK:
                bt_leave(PAGE_HOME); break;
            case APP_KEY_KEY0_QUAD:
            case APP_KEY_MENU:
            case APP_KEY_PAGE_NEXT: open_settings(); break;
            default: break;
            }
        }
        s_dirty = true;
        return;
    }
    if (s_page == PAGE_LIBRARY) {
        switch (key) {
        case APP_KEY_KEY0_SINGLE:
        case APP_KEY_DOWN:
        case APP_KEY_ROT_CW: library_move(1); break;
        case APP_KEY_KEY0_TRIPLE:
        case APP_KEY_UP:
        case APP_KEY_ROT_CCW: library_move(-1); break;
        case APP_KEY_KEY0_DOUBLE:
        case APP_KEY_ENTER:
        case APP_KEY_PLAY: library_play(s_lib_selected); break;
        case APP_KEY_KEY0_LONG:
            if (s_lib_selected >= 0) toggle_favorite(s_lib_selected);
            else s_page = PAGE_HOME;
            break;
        case APP_KEY_BACK:
        case APP_KEY_PAGE_PREV: s_page = PAGE_HOME; break;
        case APP_KEY_KEY0_QUAD:
        case APP_KEY_MENU:
        case APP_KEY_PAGE_NEXT: open_category(); break;
        default: break;
        }
        s_dirty = true;
        return;
    }
    if (s_page == PAGE_CATEGORY) {
        switch (key) {
        case APP_KEY_KEY0_SINGLE:
        case APP_KEY_DOWN:
        case APP_KEY_ROT_CW:
            if (s_category_in_group)
                collection_move(&s_category_track_selected,
                                &s_category_track_top,
                                s_category_track_count, LIB_ROWS, 1);
            else collection_move(&s_category_selected, &s_category_top,
                                 s_category_group_count, CATEGORY_ROWS, 1);
            break;
        case APP_KEY_KEY0_TRIPLE:
        case APP_KEY_UP:
        case APP_KEY_ROT_CCW:
            if (s_category_in_group)
                collection_move(&s_category_track_selected,
                                &s_category_track_top,
                                s_category_track_count, LIB_ROWS, -1);
            else collection_move(&s_category_selected, &s_category_top,
                                 s_category_group_count, CATEGORY_ROWS, -1);
            break;
        case APP_KEY_KEY0_DOUBLE:
        case APP_KEY_ENTER:
        case APP_KEY_PLAY: category_activate(); break;
        case APP_KEY_KEY0_LONG:
            if (s_category_in_group) {
                if (s_category_track_selected >= 0)
                    toggle_favorite(s_category_tracks[s_category_track_selected]);
                else category_back();
            } else {
                s_category_mode = (s_category_mode + 1) % CATEGORY_MODES;
                category_rebuild();
            }
            break;
        case APP_KEY_BACK:
        case APP_KEY_PAGE_PREV: category_back(); break;
        case APP_KEY_KEY0_QUAD:
        case APP_KEY_MENU:
        case APP_KEY_PAGE_NEXT: open_favorites(); break;
        default: break;
        }
        s_dirty = true;
        return;
    }
    if (s_page == PAGE_FAVORITES) {
        switch (key) {
        case APP_KEY_KEY0_SINGLE:
        case APP_KEY_DOWN:
        case APP_KEY_ROT_CW:
            collection_move(&s_favorite_selected, &s_favorite_top,
                            s_favorite_count, LIB_ROWS, 1);
            break;
        case APP_KEY_KEY0_TRIPLE:
        case APP_KEY_UP:
        case APP_KEY_ROT_CCW:
            collection_move(&s_favorite_selected, &s_favorite_top,
                            s_favorite_count, LIB_ROWS, -1);
            break;
        case APP_KEY_KEY0_DOUBLE:
        case APP_KEY_ENTER:
        case APP_KEY_PLAY: favorites_activate(); break;
        case APP_KEY_KEY0_LONG:
            if (s_favorite_selected >= 0)
                toggle_favorite(s_favorite_tracks[s_favorite_selected]);
            else open_category();
            break;
        case APP_KEY_BACK:
        case APP_KEY_PAGE_PREV: open_category(); break;
        case APP_KEY_KEY0_QUAD:
        case APP_KEY_MENU:
        case APP_KEY_PAGE_NEXT: open_bt(); break;
        default: break;
        }
        s_dirty = true;
        return;
    }
    switch (key) {
    case APP_KEY_PLAY:
    case APP_KEY_KEY0_SINGLE: player_toggle(); break;
    case APP_KEY_NEXT:
    case APP_KEY_KEY0_DOUBLE: player_next(); break;
    case APP_KEY_PREV:
    case APP_KEY_KEY0_TRIPLE: player_prev(); break;
    case APP_KEY_KEY0_LONG: cycle_playback_mode(); break;
    case APP_KEY_KEY0_QUAD:
    case APP_KEY_MENU:
    case APP_KEY_ENTER:
    case APP_KEY_PAGE_NEXT: open_library(); break;
    case APP_KEY_PAGE_PREV: open_settings(); break;
    case APP_KEY_UP:
    case APP_KEY_ROT_CW:
        app_settings_volume_step(1, 4);
        bt_audio_set_absolute_volume(app_settings_get()->volume);
        return;
    case APP_KEY_DOWN:
    case APP_KEY_ROT_CCW:
        app_settings_volume_step(-1, 4);
        bt_audio_set_absolute_volume(app_settings_get()->volume);
        return;
    default: return;
    }
    s_dirty = true;
}

static uint32_t seek_position(uint16_t x)
{
    int bar_x = (int)x - 261;
    if (bar_x < 0) bar_x = 0;
    if (bar_x > 188) bar_x = 188;
    return (uint32_t)((uint64_t)bar_x * player_get_dur_ms() / 188);
}

static int swipe_rows(int dy, int row_height)
{
    int distance = abs(dy);
    int rows = (distance + row_height / 2) / row_height;
    if (rows < 1) rows = 1;
    return dy < 0 ? rows : -rows;
}

static void handle_touch_sample(const touch_sample_t *sample)
{
    sync_library_generation();
    if (sample->event == LCD_TOUCH_POINT) {
        s_last_activity_us = esp_timer_get_time();
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start_x = sample->x;
            s_touch_start_y = sample->y;
            s_touch_page = s_page;
            s_touch_capture = TOUCH_CAPTURE_NONE;
            if (s_screen_asleep) {
                s_screen_asleep = false;
                s_touch_capture = TOUCH_CAPTURE_WAKE;
                s_dirty = true;
            } else if (s_page == PAGE_SETTINGS &&
                       lcd_settings_touch_begin(sample->x, sample->y)) {
                s_touch_capture = TOUCH_CAPTURE_VOLUME;
                s_dirty = true;
            } else if (s_page == PAGE_HOME && player_get_dur_ms() && player_can_seek() &&
                       sample->x >= 257 && sample->x <= 451 &&
                       sample->y >= 216 && sample->y <= 249) {
                s_touch_capture = TOUCH_CAPTURE_SEEK;
                s_seek_preview_ms = seek_position(sample->x);
                s_home_progress_dirty = true;
            }
        }
        s_touch_last_x = sample->x;
        s_touch_last_y = sample->y;
        if (s_touch_page != s_page) return;
        if (s_touch_capture == TOUCH_CAPTURE_VOLUME &&
            lcd_settings_touch_move(sample->x, sample->y)) s_settings_volume_dirty = true;
        else if (s_touch_capture == TOUCH_CAPTURE_SEEK) {
            uint32_t pos = seek_position(sample->x);
            if (pos != s_seek_preview_ms) {
                s_seek_preview_ms = pos;
                s_home_progress_dirty = true;
            }
        }
        return;
    }
    if (sample->event != LCD_TOUCH_RELEASE || !s_touch_down) return;

    if (!sample->cancelled) {
        s_touch_last_x = sample->x;
        s_touch_last_y = sample->y;
    }
    int dx = (int)s_touch_last_x - s_touch_start_x;
    int dy = (int)s_touch_last_y - s_touch_start_y;
    int adx = abs(dx), ady = abs(dy);
    bool same_page = s_touch_page == s_page;
    touch_capture_t capture = s_touch_capture;
    s_touch_down = false;
    s_touch_capture = TOUCH_CAPTURE_NONE;
    if (capture == TOUCH_CAPTURE_VOLUME) {
        lcd_settings_touch_end();
    } else if (capture == TOUCH_CAPTURE_SEEK) {
        if (same_page && !sample->cancelled) player_seek_ms(s_seek_preview_ms);
        s_dirty = true;
    } else if (capture == TOUCH_CAPTURE_NONE && same_page && !sample->cancelled) {
        if (adx >= TOUCH_PAGE_MIN && adx > ady * 3 / 2) {
            if (!s_bt_confirm_forget || s_page != PAGE_BT)
                handle_key(dx < 0 ? APP_KEY_PAGE_NEXT : APP_KEY_PAGE_PREV);
        } else if (ady >= TOUCH_SWIPE_MIN && ady > adx * 3 / 2) {
            if (s_page == PAGE_SETTINGS && s_touch_start_y >= 56)
                lcd_settings_scroll((dy < 0 ? 1 : -1) * ((ady + 51) / 52));
            else if (s_page == PAGE_LIBRARY && s_touch_start_y >= LIB_ROW_Y)
                library_scroll(swipe_rows(dy, LIB_ROW_H));
            else if (s_page == PAGE_CATEGORY &&
                     s_touch_start_y >= (s_category_in_group ? LIB_ROW_Y : CATEGORY_ROW_Y)) {
                if (s_category_in_group)
                    collection_scroll(&s_category_track_selected,
                                      &s_category_track_top,
                                      s_category_track_count, LIB_ROWS,
                                      swipe_rows(dy, LIB_ROW_H));
                else collection_scroll(&s_category_selected, &s_category_top,
                                       s_category_group_count, CATEGORY_ROWS,
                                       swipe_rows(dy, CATEGORY_ROW_H));
            } else if (s_page == PAGE_FAVORITES &&
                       s_touch_start_y >= LIB_ROW_Y)
                collection_scroll(&s_favorite_selected, &s_favorite_top,
                                  s_favorite_count, LIB_ROWS,
                                  swipe_rows(dy, LIB_ROW_H));
            else if (s_page == PAGE_BT && !s_bt_confirm_forget &&
                     s_touch_start_y >= 132)
                bt_scroll(swipe_rows(dy, BT_ROW_H));
            s_dirty = true;
        } else if (adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
            handle_touch(s_touch_start_x, s_touch_start_y);
        }
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    bool contact = false;
    bool point_queued = false;
    int64_t last_point_us = 0;
    uint16_t last_x = 0, last_y = 0;
    uint16_t queued_x = 0, queued_y = 0;
    for (;;) {
        uint16_t x = 0, y = 0;
        lcd_touch_event_t event = lcd_touch_poll(&x, &y);
        int64_t now = esp_timer_get_time();
        if (event == LCD_TOUCH_POINT) {
            if (!point_queued || x != queued_x || y != queued_y) {
                touch_sample_t sample = {.event = event, .x = x, .y = y};
                if (xQueueSend(s_touch_queue, &sample, 0) == pdTRUE) {
                    point_queued = true;
                    queued_x = x;
                    queued_y = y;
                }
            }
            contact = true;
            last_point_us = now;
            last_x = x;
            last_y = y;
        } else if (event == LCD_TOUCH_RELEASE ||
                   (contact && now - last_point_us > 2000000)) {
            if (contact) {
                touch_sample_t sample = {
                    .event = LCD_TOUCH_RELEASE, .x = last_x, .y = last_y,
                    .cancelled = event != LCD_TOUCH_RELEASE,
                };
                if (xQueueSend(s_touch_queue, &sample, pdMS_TO_TICKS(100)) != pdTRUE) {
                    xQueueReset(s_touch_queue);
                    sample.cancelled = true;
                    xQueueSend(s_touch_queue, &sample, 0);
                }
            }
            contact = false;
            point_queued = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    bool backlight_on = false;
    for (;;) {
        sync_library_generation();
        if (lcd_cover_thumb_take_update() &&
            (s_page == PAGE_LIBRARY || s_page == PAGE_FAVORITES ||
             (s_page == PAGE_CATEGORY && s_category_in_group))) s_dirty = true;
        app_evt_t event;
        while (app_evt_wait(&event, 0)) {
            if (event.id == APP_EVT_INPUT) {
                s_last_activity_us = esp_timer_get_time();
                if (s_screen_asleep) {
                    s_screen_asleep = false;
                    s_dirty = true;
                } else {
                    handle_key((app_key_t)event.input.key);
                }
            } else if (event.id == APP_EVT_SETTINGS) {
                if (s_panel) lcd_panel_set_brightness(app_settings_get()->brightness);
                s_dirty = true;
            }
            else if (event.id == APP_EVT_LIB_READY) {
                sync_library_generation();
                s_dirty = true;
            } else if (event.id == APP_EVT_TRACK_POS) {
                if (s_page == PAGE_HOME) s_home_progress_dirty = true;
            } else if (event.id == APP_EVT_BT_VOLUME) {
                if (s_page == PAGE_SETTINGS) s_settings_volume_dirty = true;
            } else if (event.id != APP_EVT_AUDIO_LEVEL &&
                       event.id != APP_EVT_LIB_PROGRESS &&
                       event.id != APP_EVT_UPLOAD_PROGRESS) {
                /* These progress notifications arrive many times per second and
                 * no visible page renders them; skip the full repaint/flush. */
                s_dirty = true;
            }
        }
        touch_sample_t touch;
        while (s_touch_queue && xQueueReceive(s_touch_queue, &touch, 0) == pdTRUE)
            handle_touch_sample(&touch);
        if (s_page == PAGE_BT) {
            if (s_bt_cancel_sent && bt_audio_state() != BT_ST_CONNECTING) {
                s_bt_cancel_sent = false;
                s_dirty = true;
            }
            if (s_bt_scan_pending && bt_audio_state() == BT_ST_READY &&
                !bt_audio_discovering()) {
                s_bt_scan_pending = false;
                if (bt_audio_start_discovery(0) != ESP_OK) bt_notice("搜索启动失败");
            }
            bt_forget_status_t status = bt_audio_forget_status();
            if (status != s_bt_last_forget_status) {
                s_bt_last_forget_status = status;
                if (status == BT_FORGET_DONE) bt_notice("已忘记设备");
                else if (status == BT_FORGET_FAILED) bt_notice("忘记设备失败");
                s_dirty = true;
            }
            if (s_bt_notice[0] && esp_timer_get_time() >= s_bt_notice_until_us) {
                s_bt_notice[0] = '\0';
                s_dirty = true;
            }
            if (bt_audio_state() == BT_ST_DISCOVERING &&
                esp_timer_get_time() - s_bt_anim_us >= 600000) {
                s_bt_anim_us = esp_timer_get_time();
                s_bt_anim_phase = (s_bt_anim_phase + 1) % 3;
                s_dirty = true;
            }
        }
        if (s_page == PAGE_SETTINGS && lcd_settings_tick()) s_dirty = true;
        const music_item_t *item = player_get_current();
        int64_t now = esp_timer_get_time();
        if (s_page == PAGE_HOME &&
            strcmp(item ? item->path : "", s_track_path) != 0) s_dirty = true;
        uint8_t timeout = app_settings_get()->screen_timeout_s;
        if (!timeout && s_screen_asleep) {
            s_screen_asleep = false;
            s_dirty = true;
        } else if (timeout && !s_screen_asleep &&
                   now - s_last_activity_us >= (int64_t)timeout * 1000000) {
            s_screen_asleep = true;
            if (backlight_on) { lcd_panel_backlight(false); backlight_on = false; }
        }
        bool title_scroll = s_panel && s_page == PAGE_HOME && item &&
                            lcd_text_width(item->title, 24) > HOME_TITLE_W;
        bool touch_frame_due = !s_touch_down ||
                               esp_timer_get_time() - s_last_touch_frame_us >= UI_FRAME_US;
        if (s_panel && s_dirty && !s_screen_asleep && touch_frame_due) {
            int64_t frame_start_us = esp_timer_get_time();
            if (s_page == PAGE_LIBRARY) draw_library();
            else if (s_page == PAGE_CATEGORY) draw_category();
            else if (s_page == PAGE_FAVORITES) draw_favorites();
            else if (s_page == PAGE_BT) draw_bt();
            else if (s_page == PAGE_SETTINGS) lcd_settings_draw(s_frame);
            else draw_home();
            if (lcd_panel_flush(s_frame) == ESP_OK) {
                if (!backlight_on) { lcd_panel_backlight(true); backlight_on = true; }
                s_dirty = false;
                s_home_progress_dirty = false;
                s_settings_volume_dirty = false;
                s_last_scroll_frame_us = esp_timer_get_time();
                if (s_touch_down) s_last_touch_frame_us = frame_start_us;
            } else {
                ESP_LOGW(TAG, "display transfer failed");
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else if (s_panel && s_page == PAGE_SETTINGS &&
                   s_settings_volume_dirty && !s_screen_asleep && touch_frame_due) {
            int top;
            int64_t frame_start_us = esp_timer_get_time();
            if (!lcd_settings_draw_volume(s_frame, &top)) {
                s_settings_volume_dirty = false;
            } else if (lcd_panel_flush_rect(s_frame, 181, top, 281, 40) == ESP_OK) {
                s_settings_volume_dirty = false;
                if (s_touch_down) s_last_touch_frame_us = frame_start_us;
            } else {
                s_dirty = true;
            }
        } else if (s_panel && s_page == PAGE_HOME && !s_screen_asleep) {
            lcd_gfx_bind(s_frame);
            uint32_t lyric_pos = player_get_pos_ms();
            bool seeking = s_touch_down && s_touch_capture == TOUCH_CAPTURE_SEEK;
            if (seeking) lyric_pos = s_seek_preview_ms;
            if (lcd_lyrics_available() && home_lyrics_changed(lyric_pos)) {
                int64_t delta = (int64_t)lyric_pos - s_last_lyric_pos;
                if (!seeking && delta >= 0 && delta < 500 && s_lyric_frames &&
                    s_last_lyric_current) {
                    start_lyric_scroll(lyric_pos);
                } else {
                    s_lyric_scrolling = false;
                    draw_home_lyrics(lyric_pos);
                    if (lcd_panel_flush_rect(s_frame, LYRIC_X, LYRIC_Y,
                                             LYRIC_W, LYRIC_H) != ESP_OK) s_dirty = true;
                }
            }
            s_last_lyric_pos = lyric_pos;
            if (s_lyric_scrolling &&
                esp_timer_get_time() - s_last_lyric_frame_us >= UI_FRAME_US) {
                if (!draw_lyric_scroll_frame(esp_timer_get_time())) {
                    s_lyric_scrolling = false;
                    s_dirty = true;
                }
            }
            if (s_home_progress_dirty && touch_frame_due) {
                int64_t frame_start_us = esp_timer_get_time();
                uint32_t pos = player_get_pos_ms();
                uint32_t dur = player_get_dur_ms();
                if (s_touch_down && s_touch_capture == TOUCH_CAPTURE_SEEK)
                    pos = s_seek_preview_ms;
                if (dur && pos > dur) pos = dur;
                draw_home_progress(item, pos, dur);
                bool updated = lcd_panel_flush_rect(s_frame, 261, 226, 188,
                                                    HOME_PROGRESS_H) == ESP_OK;
                if (updated) {
                    s_home_progress_dirty = false;
                    if (s_touch_down) s_last_touch_frame_us = frame_start_us;
                }
                else s_dirty = true;
            }
            if (!s_dirty && title_scroll &&
                esp_timer_get_time() - s_last_scroll_frame_us >= HOME_TITLE_FRAME_US) {
                int64_t frame_start_us = esp_timer_get_time();
                draw_home_title(item);
                if (lcd_panel_flush_rect(s_frame, HOME_TITLE_X, HOME_TITLE_Y,
                                         HOME_TITLE_W, lcd_text_height(24)) == ESP_OK)
                    s_last_scroll_frame_us = frame_start_us;
                else s_dirty = true;
            }
        }
        if (s_panel && s_page == PAGE_HOME && s_track_assets_pending &&
            (player_get_state() == PLAYER_PLAYING ||
             player_get_state() == PLAYER_PAUSED ||
             player_get_state() == PLAYER_ERROR) &&
            !s_dirty && !s_screen_asleep) {
            s_track_assets_pending = false;
            lcd_lyrics_load(s_track_path);
            lcd_cover_load(s_track_path);
            s_dirty = true;
        }
        if (s_panel && !s_dirty && !s_touch_down && !s_screen_asleep &&
            !music_lib_scanning() &&
            esp_timer_get_time() - s_last_list_scroll_us >= THUMB_IDLE_US &&
            (s_page == PAGE_LIBRARY || s_page == PAGE_FAVORITES ||
             (s_page == PAGE_CATEGORY && s_category_in_group))) {
            for (int row = 0; row < LIB_ROWS; row++) {
                int index = -1;
                if (s_page == PAGE_LIBRARY && s_lib_top + row < music_lib_count())
                    index = s_lib_top + row;
                else if (s_page == PAGE_FAVORITES &&
                         s_favorite_top + row < s_favorite_count)
                    index = s_favorite_tracks[s_favorite_top + row];
                else if (s_page == PAGE_CATEGORY && s_category_in_group &&
                         s_category_track_top + row < s_category_track_count)
                    index = s_category_tracks[s_category_track_top + row];
                if (index < 0) break;
                const music_item_t *visible = music_lib_get(index);
                if (visible && lcd_cover_thumb_prepare(row, visible->path)) break;
            }
        }
        int delay_ms = s_touch_down ? 10 : 20;
        if (s_touch_down && (s_dirty || s_home_progress_dirty ||
                             s_settings_volume_dirty)) {
            int64_t until_frame_us = s_last_touch_frame_us + UI_FRAME_US -
                                     esp_timer_get_time();
            int frame_delay_ms = until_frame_us > 1000 ?
                                 (int)(until_frame_us / 1000) : 1;
            if (frame_delay_ms < delay_ms) delay_ms = frame_delay_ms;
        }
        if (title_scroll && !s_screen_asleep) {
            int64_t until_frame_us = s_last_scroll_frame_us + HOME_TITLE_FRAME_US -
                                     esp_timer_get_time();
            int title_delay_ms = until_frame_us > 1000 ?
                                 (int)(until_frame_us / 1000) : 1;
            if (title_delay_ms > 10) title_delay_ms = 10;
            if (title_delay_ms < delay_ms) delay_ms = title_delay_ms;
        }
        if (s_lyric_scrolling && s_page == PAGE_HOME && !s_screen_asleep) {
            int64_t until_frame_us = s_last_lyric_frame_us + UI_FRAME_US -
                                     esp_timer_get_time();
            int lyric_delay_ms = until_frame_us > 1000 ?
                                 (int)(until_frame_us / 1000) : 1;
            if (lyric_delay_ms < delay_ms) delay_ms = lyric_delay_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t ui_init(void)
{
    s_scroll_origin_us = esp_timer_get_time();
    s_last_activity_us = s_scroll_origin_us;
    s_frame = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        ESP_LOGE(TAG, "framebuffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_lyric_frames = heap_caps_malloc((size_t)LYRIC_W * LYRIC_H *
                                     2 * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lyric_frames) ESP_LOGW(TAG, "lyric animation disabled: no PSRAM");
    if (!lcd_cover_thumb_init()) ESP_LOGW(TAG, "background thumbnails unavailable");
    esp_err_t err = lcd_panel_init();
    s_panel = err == ESP_OK;
    if (!s_panel) ESP_LOGW(TAG, "LCD unavailable: %s", esp_err_to_name(err));
    else lcd_panel_set_brightness(app_settings_get()->brightness);
    if (lcd_touch_init() == ESP_OK) {
        s_touch_queue = xQueueCreate(48, sizeof(touch_sample_t));
        if (!s_touch_queue ||
            xTaskCreate(touch_task, "lcd_touch", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "touch task unavailable; KEY0 still works");
            if (s_touch_queue) vQueueDelete(s_touch_queue);
            s_touch_queue = NULL;
        }
    } else ESP_LOGW(TAG, "touch unavailable; KEY0 still works");
#if CONFIG_FREERTOS_UNICORE
    BaseType_t ok = xTaskCreate(ui_task, "lcd_ui", 24 * 1024, NULL, 4, NULL);
#else
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "lcd_ui", 24 * 1024,
                                            NULL, 4, NULL, 1);
#endif
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool ui_has_panel(void)
{
    return s_panel;
}
