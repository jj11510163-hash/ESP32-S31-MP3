/*
 * mp3_dec.h —— Helix MP3 解码器封装（内部头文件）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int samplerate;
    int channels;
    int bitrate;   /* kbps */
    int frame_samples;
    int32_t file_size;
    int32_t audio_offset; /* 跳过 ID3 后的起始偏移 */
} mp3_info_t;

typedef struct mp3_dec mp3_dec_t;

mp3_dec_t *mp3_open(const char *path, mp3_info_t *out_info);
/* 解一帧；返回每声道样本数，0 = EOF，<0 = 错误 */
int mp3_next(mp3_dec_t *d, int16_t *pcm, int max_samples);
void mp3_close(mp3_dec_t *d);
uint32_t mp3_tell_ms(mp3_dec_t *d);
bool mp3_seek_ms(mp3_dec_t *d, uint32_t ms);
uint32_t mp3_duration_ms(const mp3_dec_t *d);

/* 轻量探测：只解码一帧，拿到时长/码率/采样率（扫描曲库用） */
bool mp3_probe(const char *path, uint32_t *dur_ms, uint16_t *bitrate, uint16_t *srate, uint8_t *channels);
