/* aac_enc.h —— esp_audio_codec 的 AAC-LC 编码器封装 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct aac_enc aac_enc_t;

aac_enc_t *aac_enc_open(int sample_rate, int channels, int bitrate);
int aac_enc_frame_samples(const aac_enc_t *e); /* 输入帧（样本/声道），AAC-LC 1024 */
int aac_enc_max_output(const aac_enc_t *e);
/* 返回编码后字节数，<0 失败 */
int aac_enc_encode(aac_enc_t *e, const int16_t *pcm, int frames, uint8_t *out, int out_cap);
void aac_enc_close(aac_enc_t *e);
