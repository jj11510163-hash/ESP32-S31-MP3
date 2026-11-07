/*
 * decoder.h —— 统一音频解码器接口（内部头文件）
 *
 * 播放器只认这一层，新增格式只要实现一个 vtable 并在 decoder.c 里登记。
 * 约定：所有解码器输出 **16bit 立体声交错 PCM**（单声道会自动复制成双声道），
 *       采样率保持源采样率，由播放器统一重采样到 44.1kHz。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "music.h"

typedef struct audio_dec audio_dec_t;

typedef struct {
    int samplerate;
    int channels;  /* 源声道数 1/2 */
    int bits;      /* 源位深 */
    uint32_t duration_ms;
    music_fmt_t fmt;
} audio_info_t;

typedef struct audio_decoder_vtbl {
    int (*read)(audio_dec_t *d, int16_t *pcm, int max_frames); /* 返回帧数，0=EOF，<0=错误 */
    bool (*seek_ms)(audio_dec_t *d, uint32_t ms);
    uint32_t (*tell_ms)(audio_dec_t *d);
    void (*close)(audio_dec_t *d);
} audio_decoder_vtbl_t;

struct audio_dec {
    void *impl;
    const audio_decoder_vtbl_t *vt;
    audio_info_t info;
};

/* 打开任意支持的格式；hint 可以为 MUSIC_FMT_UNKNOWN，此时按扩展名+文件头判断 */
audio_dec_t *audio_dec_open(const char *path, music_fmt_t hint);

int audio_dec_read(audio_dec_t *d, int16_t *pcm, int max_frames);
bool audio_dec_seek_ms(audio_dec_t *d, uint32_t ms);
uint32_t audio_dec_tell_ms(audio_dec_t *d);
void audio_dec_close(audio_dec_t *d);

const char *audio_dec_format_name(music_fmt_t fmt);

/* 各格式后端（decoder.c 里登记） */
audio_dec_t *dec_mp3_open(const char *path);
audio_dec_t *dec_flac_open(const char *path);
audio_dec_t *dec_wav_open(const char *path);
audio_dec_t *dec_aac_open(const char *path, music_fmt_t fmt);
bool dec_aac_init(void);

/* 由具体解码器填写，统一分配 audio_dec_t 外壳 */
audio_dec_t *audio_dec_alloc(const audio_decoder_vtbl_t *vt, size_t impl_size, void **impl_out);
