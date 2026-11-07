/*
 * music.h —— SD 卡曲库 + MP3 播放管道
 *
 * 数据流：SD 文件 -> 读取缓冲(PSRAM) -> helix MP3 解码 -> PCM 环形缓冲(PSRAM)
 *        -> A2DP source 数据回调 (player_read_pcm) -> 经典蓝牙 -> 耳机
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_PATH_MAX 256
#define MUSIC_SCAN_DIR "/sdcard/music"

/* 支持的容器/编码 */
typedef enum {
    MUSIC_FMT_UNKNOWN = 0,
    MUSIC_FMT_MP3,   /* MPEG-1/2 Layer III，helix 定点解码 */
    MUSIC_FMT_FLAC,  /* Free Lossless Audio Codec，dr_flac */
    MUSIC_FMT_WAV,   /* PCM 8/16/24/32 + IEEE float32 */
    MUSIC_FMT_AAC,   /* ADTS AAC */
    MUSIC_FMT_M4A,   /* AAC in MPEG-4 audio container */
} music_fmt_t;

typedef struct {
    char path[MUSIC_PATH_MAX];
    char title[64];
    char artist[40];
    char album[40];
    char publisher[48];
    uint32_t size;
    uint32_t duration_ms; /* 0 = 未知（需解码首帧估算） */
    uint16_t year;
    uint16_t track_no;
    music_fmt_t fmt;
    uint32_t samplerate;
    uint8_t bits; /* 源位深 8/16/24/32 */
    uint8_t channels;
} music_item_t;

music_fmt_t music_fmt_from_path(const char *path);
const char *music_fmt_name(music_fmt_t fmt);

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_LOADING,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ERROR,
} player_state_t;

/* ------------------------------ 曲库 ------------------------------ */
esp_err_t music_lib_init(void);
esp_err_t music_lib_scan(void);          /* 同步扫描（供后台任务调用） */
esp_err_t music_lib_rescan_async(void);  /* 起任务异步扫描 */
/* Only one maintenance mode may own the SD card at a time. */
typedef enum {
    MUSIC_SD_OWNER_NONE = 0,
    MUSIC_SD_OWNER_OTA,
} music_sd_owner_t;

bool music_sd_try_acquire(music_sd_owner_t owner);
void music_sd_release(music_sd_owner_t owner);
music_sd_owner_t music_sd_owner(void);
bool music_update_mode(void);
int music_lib_count(void);
const music_item_t *music_lib_get(int index);
bool music_lib_scanning(void);
uint32_t music_lib_generation(void);     /* 完成一次曲库重扫后递增 */

/* 收藏以 SD 路径保存；索引仅包含当前曲库中仍存在的歌曲。 */
esp_err_t music_favorites_init(void);
bool music_favorites_contains(const char *path);
esp_err_t music_favorites_toggle(const char *path, bool *now_favorite);
int music_favorites_count(void);
int music_favorites_index_at(int position);
esp_err_t music_favorites_refresh(void);
/* Wait for any in-flight favorites file operation before SD is unmounted. */
void music_favorites_wait_idle(void);

/* ------------------------------ 播放器 ------------------------------ */
esp_err_t player_init(void);
esp_err_t player_start_index(int index, bool auto_advance);
esp_err_t player_start_collection(int index, const int *indices, int count, bool auto_advance);
esp_err_t player_toggle(void);
esp_err_t player_pause(void);
esp_err_t player_resume(void);
esp_err_t player_stop(void);
esp_err_t player_next(void);
esp_err_t player_prev(void);
esp_err_t player_seek_ms(uint32_t ms);
bool player_can_seek(void);

player_state_t player_get_state(void);
const music_item_t *player_get_current(void);
uint32_t player_get_pos_ms(void);
uint32_t player_get_dur_ms(void);
const char *player_state_name(player_state_t st);

/* A2DP 回调从环形缓冲取 PCM（16bit 立体声，44.1kHz） */
int32_t player_read_pcm(uint8_t *buf, int32_t len);
/* 环形缓冲水位百分比，用于 UI 抖动提示 */
uint8_t player_buffer_level(void);

/* 音频可视化：返回若干段能量 0..100（内部约 30ms 更新一次，可直接喂 VU 表） */
uint8_t player_get_bands(uint8_t *bands, uint8_t max_bands);
/* 当前峰值 0..100（带衰减），用于峰顶指示 */
uint8_t player_get_peak(void);

#ifdef __cplusplus
}
#endif
