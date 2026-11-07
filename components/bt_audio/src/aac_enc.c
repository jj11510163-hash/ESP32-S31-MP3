#include "aac_enc.h"

#include <stdlib.h>
#include <string.h>

#include "esp_aac_enc.h"
#include "esp_audio_enc.h"
#include "esp_log.h"

static const char *TAG = "aac_enc";

struct aac_enc {
    esp_audio_enc_handle_t hdl;
    int in_bytes;   /* 一帧输入字节数 */
    int in_frames;  /* 一帧样本数/声道 */
    int out_bytes;  /* 一帧最大输出 */
};

static bool s_registered;

aac_enc_t *aac_enc_open(int sample_rate, int channels, int bitrate)
{
    if (!s_registered) {
        if (esp_aac_enc_register() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_aac_enc_register failed");
            return NULL;
        }
        s_registered = true;
    }

    aac_enc_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }

    esp_aac_enc_config_t cfg = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = bitrate,
        .adts_used = false, /* A2DP 用 raw AAC，LATM 由协议栈封装 */
    };
    esp_audio_enc_config_t ec = {
        .type = ESP_AUDIO_TYPE_AAC,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    if (esp_audio_enc_open(&ec, &e->hdl) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "aac encoder open failed (%dHz %dch %dbps)", sample_rate, channels, bitrate);
        free(e);
        return NULL;
    }

    int in_sz = 0, out_sz = 0;
    if (esp_audio_enc_get_frame_size(e->hdl, &in_sz, &out_sz) != ESP_AUDIO_ERR_OK) {
        esp_audio_enc_close(e->hdl);
        free(e);
        return NULL;
    }
    e->in_bytes = in_sz;
    e->in_frames = in_sz / (int)(sizeof(int16_t) * (uint32_t)channels);
    e->out_bytes = out_sz;

    ESP_LOGI(TAG, "AAC encoder: %dHz %dch %dbps, frame %d samples, out<=%d", sample_rate, channels,
             bitrate, e->in_frames, e->out_bytes);
    return e;
}

int aac_enc_frame_samples(const aac_enc_t *e)
{
    return e ? e->in_frames : 0;
}

int aac_enc_max_output(const aac_enc_t *e)
{
    return e ? e->out_bytes : 0;
}

int aac_enc_encode(aac_enc_t *e, const int16_t *pcm, int frames, uint8_t *out, int out_cap)
{
    if (!e || !pcm || !out) {
        return -1;
    }
    if (frames > e->in_frames) {
        frames = e->in_frames;
    }
    if (out_cap < e->out_bytes) {
        return -1;
    }

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)frames * 2 * 2,
    };
    esp_audio_enc_out_frame_t o = {
        .buffer = out,
        .len = (uint32_t)e->out_bytes,
    };
    esp_audio_err_t ret = esp_audio_enc_process(e->hdl, &in, &o);
    if (ret != ESP_AUDIO_ERR_OK || o.encoded_bytes == 0) {
        return -1;
    }
    return (int)o.encoded_bytes;
}

void aac_enc_close(aac_enc_t *e)
{
    if (!e) {
        return;
    }
    if (e->hdl) {
        esp_audio_enc_close(e->hdl);
    }
    free(e);
}
