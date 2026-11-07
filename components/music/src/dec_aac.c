#include "decoder.h"
#include "m4a_tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_audio_simple_dec.h"
#include "esp_aac_dec.h"
#include "impl/esp_m4a_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "aac_file";

#define INPUT_START_SIZE 8192
#define INPUT_MAX_SIZE (1024 * 1024)
#define OUTPUT_START_SIZE 16384
#define OUTPUT_MAX_SIZE (64 * 1024)

typedef struct {
    FILE *file;
    esp_audio_simple_dec_handle_t decoder;
    uint8_t *input;
    uint8_t *output;
    size_t input_size;
    size_t output_size;
    size_t input_pos;
    size_t input_len;
    size_t output_pos;
    size_t output_len;
    long start;
    uint64_t frames_read;
    bool eof;
    bool flushed;
} aac_impl_t;

static bool s_aac_ready;
static bool s_m4a_ready;

bool dec_aac_init(void)
{
    if (!s_aac_ready) {
        esp_audio_err_t err = esp_aac_dec_register();
        if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "AAC decoder registration failed: %d", err);
            return false;
        }
        s_aac_ready = true;
    }
    if (!s_m4a_ready) {
        esp_audio_err_t err = esp_m4a_dec_register();
        if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGW(TAG, "M4A decoder registration failed: %d", err);
        } else {
            s_m4a_ready = true;
        }
    }
    return s_aac_ready;
}

static long audio_start(FILE *file)
{
    uint8_t head[10];
    if (fread(head, 1, sizeof(head), file) == sizeof(head) &&
        memcmp(head, "ID3", 3) == 0) {
        uint32_t size = ((uint32_t)(head[6] & 0x7F) << 21) |
                        ((uint32_t)(head[7] & 0x7F) << 14) |
                        ((uint32_t)(head[8] & 0x7F) << 7) | (head[9] & 0x7F);
        long start = 10 + (long)size + ((head[5] & 0x10) ? 10 : 0);
        if (fseek(file, start, SEEK_SET) == 0) return start;
    }
    fseek(file, 0, SEEK_SET);
    return 0;
}

static bool grow_buffer(uint8_t **buffer, size_t *capacity, size_t maximum)
{
    if (*capacity >= maximum) return false;
    size_t next = *capacity * 2;
    if (next > maximum) next = maximum;
    uint8_t *new_buffer = heap_caps_realloc(*buffer, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_buffer) return false;
    *buffer = new_buffer;
    *capacity = next;
    return true;
}

static bool append_input(aac_impl_t *im)
{
    if (im->input_pos) {
        memmove(im->input, im->input + im->input_pos, im->input_len - im->input_pos);
        im->input_len -= im->input_pos;
        im->input_pos = 0;
    }
    if (im->input_len == im->input_size &&
        !grow_buffer(&im->input, &im->input_size, INPUT_MAX_SIZE)) return false;
    size_t got = fread(im->input + im->input_len, 1, im->input_size - im->input_len, im->file);
    im->input_len += got;
    if (got == 0 || feof(im->file)) im->eof = true;
    return got > 0;
}

static int decode_frame(audio_dec_t *ad)
{
    aac_impl_t *im = ad->impl;
    for (int attempts = 0; attempts < 1024; attempts++) {
        if (im->input_pos == im->input_len && !im->eof && !append_input(im)) {
            if (ferror(im->file)) return -1;
        }
        if (im->input_pos == im->input_len && im->eof && im->flushed) return 0;

        size_t available = im->input_len - im->input_pos;
        esp_audio_simple_dec_raw_t raw = {
            .buffer = im->input + im->input_pos,
            .len = (uint32_t)available,
            .eos = im->eof,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = im->output,
            .len = (uint32_t)im->output_size,
        };
        esp_audio_err_t err = esp_audio_simple_dec_process(im->decoder, &raw, &out);
        if (raw.consumed > available) return -1;
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size <= im->output_size || out.needed_size > OUTPUT_MAX_SIZE) return -1;
            while (im->output_size < out.needed_size) {
                if (!grow_buffer(&im->output, &im->output_size, OUTPUT_MAX_SIZE)) return -1;
            }
            continue;
        }
        im->input_pos += raw.consumed;
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "decode failed: %d", err);
            return -1;
        }
        if (out.decoded_size) {
            esp_audio_simple_dec_info_t info;
            if (esp_audio_simple_dec_get_info(im->decoder, &info) != ESP_AUDIO_ERR_OK ||
                !info.sample_rate || info.channel < 1 || info.channel > 2 ||
                info.bits_per_sample != 16 || out.decoded_size > im->output_size ||
                out.decoded_size % (info.channel * sizeof(int16_t)) != 0) {
                ESP_LOGW(TAG, "unsupported AAC PCM format");
                return -1;
            }
            if (ad->info.samplerate &&
                (ad->info.samplerate != (int)info.sample_rate ||
                 ad->info.channels != (int)info.channel)) {
                ESP_LOGW(TAG, "AAC format changed mid-stream");
                return -1;
            }
            ad->info.samplerate = (int)info.sample_rate;
            ad->info.channels = (int)info.channel;
            ad->info.bits = 16;
            im->output_pos = 0;
            im->output_len = out.decoded_size;
            return 1;
        }
        if (available == 0 && im->eof) {
            im->flushed = true;
            return 0;
        }
        if (raw.consumed == 0) {
            if (im->eof) return 0;
            if (!append_input(im)) return -1;
        }
    }
    ESP_LOGW(TAG, "AAC parser did not reach an output frame");
    return -1;
}

static int aac_read(audio_dec_t *ad, int16_t *pcm, int max_frames)
{
    aac_impl_t *im = ad->impl;
    if (!pcm || max_frames <= 0) return -1;
    int total = 0;
    while (total < max_frames) {
        if (im->output_pos == im->output_len) {
            int result = decode_frame(ad);
            if (result <= 0) return total ? total : result;
        }
        int channels = ad->info.channels;
        int available = (int)((im->output_len - im->output_pos) / (2 * channels));
        int take = max_frames - total;
        if (take > available) take = available;
        if (take <= 0) return total ? total : -1;
        const int16_t *source = (const int16_t *)(im->output + im->output_pos);
        if (channels == 2) {
            memcpy(pcm + total * 2, source, (size_t)take * 2 * sizeof(int16_t));
        } else {
            for (int i = 0; i < take; i++) {
                pcm[(total + i) * 2] = source[i];
                pcm[(total + i) * 2 + 1] = source[i];
            }
        }
        im->output_pos += (size_t)take * channels * sizeof(int16_t);
        im->frames_read += take;
        total += take;
    }
    return total;
}

static bool aac_seek(audio_dec_t *ad, uint32_t ms)
{
    (void)ad;
    (void)ms;
    return false;
}

static uint32_t aac_tell(audio_dec_t *ad)
{
    aac_impl_t *im = ad->impl;
    if (ad->info.samplerate <= 0) return 0;
    uint64_t ms = im->frames_read * 1000 / (uint32_t)ad->info.samplerate;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void aac_close(audio_dec_t *ad)
{
    aac_impl_t *im = ad->impl;
    if (im->decoder) esp_audio_simple_dec_close(im->decoder);
    if (im->file) fclose(im->file);
    free(im->input);
    free(im->output);
}

static const audio_decoder_vtbl_t s_vt = {
    .read = aac_read,
    .seek_ms = aac_seek,
    .tell_ms = aac_tell,
    .close = aac_close,
};

audio_dec_t *dec_aac_open(const char *path, music_fmt_t fmt)
{
    if (fmt != MUSIC_FMT_AAC && fmt != MUSIC_FMT_M4A) return NULL;
    if (!s_aac_ready || (fmt == MUSIC_FMT_M4A && !s_m4a_ready)) return NULL;
    void *impl = NULL;
    audio_dec_t *ad = audio_dec_alloc(&s_vt, sizeof(aac_impl_t), &impl);
    if (!ad) return NULL;
    aac_impl_t *im = impl;
    im->file = fopen(path, "rb");
    if (!im->file) goto fail;
    im->start = fmt == MUSIC_FMT_AAC ? audio_start(im->file) : 0;
    im->input_size = INPUT_START_SIZE;
    im->output_size = OUTPUT_START_SIZE;
    im->input = heap_caps_malloc(im->input_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    im->output = heap_caps_malloc(im->output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!im->input || !im->output) goto fail;

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = fmt == MUSIC_FMT_M4A ? ESP_AUDIO_SIMPLE_DEC_TYPE_M4A :
                                          ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &im->decoder) != ESP_AUDIO_ERR_OK) goto fail;

    ad->info.fmt = fmt;
    if (fmt == MUSIC_FMT_M4A) {
        m4a_tags_t tags;
        if (m4a_tags_read(path, &tags)) ad->info.duration_ms = tags.duration_ms;
    }
    if (decode_frame(ad) <= 0) goto fail;
    if (!ad->info.duration_ms && fmt == MUSIC_FMT_AAC) {
        esp_audio_simple_dec_info_t info;
        if (esp_audio_simple_dec_get_info(im->decoder, &info) == ESP_AUDIO_ERR_OK && info.bitrate) {
            long current = ftell(im->file);
            if (current >= 0 && fseek(im->file, 0, SEEK_END) == 0) {
                long end = ftell(im->file);
                if (end > im->start) {
                    uint64_t ms = (uint64_t)(end - im->start) * 8000 / info.bitrate;
                    ad->info.duration_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
                }
                fseek(im->file, current, SEEK_SET);
            }
        }
    }
    return ad;

fail:
    aac_close(ad);
    free(ad->impl);
    free(ad);
    ESP_LOGW(TAG, "unsupported AAC file: %s", path);
    return NULL;
}
