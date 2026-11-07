#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "decoder";

audio_dec_t *audio_dec_alloc(const audio_decoder_vtbl_t *vt, size_t impl_size, void **impl_out)
{
    audio_dec_t *d = calloc(1, sizeof(audio_dec_t));
    if (!d) {
        return NULL;
    }
    d->impl = calloc(1, impl_size);
    if (!d->impl) {
        free(d);
        return NULL;
    }
    d->vt = vt;
    if (impl_out) {
        *impl_out = d->impl;
    }
    return d;
}

/* 文件头嗅探，扩展名不认识时兜底 */
static music_fmt_t sniff(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return MUSIC_FMT_UNKNOWN;
    }
    uint8_t h[12] = {0};
    size_t n = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    if (n < 4) {
        return MUSIC_FMT_UNKNOWN;
    }
    if (memcmp(h, "fLaC", 4) == 0) {
        return MUSIC_FMT_FLAC;
    }
    if (memcmp(h, "RIFF", 4) == 0 && n >= 12 && memcmp(h + 8, "WAVE", 4) == 0) {
        return MUSIC_FMT_WAV;
    }
    if (n >= 12 && memcmp(h + 4, "ftyp", 4) == 0) {
        return MUSIC_FMT_M4A;
    }
    if (h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) {
        return MUSIC_FMT_AAC;
    }
    if (memcmp(h, "ID3", 3) == 0 || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)) {
        return MUSIC_FMT_MP3;
    }
    return MUSIC_FMT_UNKNOWN;
}

audio_dec_t *audio_dec_open(const char *path, music_fmt_t hint)
{
    if (path == NULL) {
        return NULL;
    }
    music_fmt_t fmt = hint;
    if (fmt == MUSIC_FMT_UNKNOWN) {
        fmt = music_fmt_from_path(path);
    }
    if (fmt == MUSIC_FMT_UNKNOWN) {
        fmt = sniff(path);
    }

    audio_dec_t *d = NULL;
    switch (fmt) {
    case MUSIC_FMT_MP3:
        d = dec_mp3_open(path);
        break;
    case MUSIC_FMT_FLAC:
        d = dec_flac_open(path);
        break;
    case MUSIC_FMT_WAV:
        d = dec_wav_open(path);
        break;
    case MUSIC_FMT_AAC:
    case MUSIC_FMT_M4A:
        d = dec_aac_open(path, fmt);
        break;
    default:
        break;
    }

    /* Retry with the detected format if the file extension is incorrect. */
    if (d == NULL) {
        music_fmt_t alt = sniff(path);
        if (alt != MUSIC_FMT_UNKNOWN && alt != fmt) {
            ESP_LOGW(TAG, "%s: fallback to %s", path, audio_dec_format_name(alt));
            switch (alt) {
            case MUSIC_FMT_MP3:
                d = dec_mp3_open(path);
                break;
            case MUSIC_FMT_FLAC:
                d = dec_flac_open(path);
                break;
            case MUSIC_FMT_WAV:
                d = dec_wav_open(path);
                break;
            case MUSIC_FMT_AAC:
            case MUSIC_FMT_M4A:
                d = dec_aac_open(path, alt);
                break;
            default:
                break;
            }
        }
    }

    if (d) {
        ESP_LOGI(TAG, "open %s: %s %dHz %dch %dbit %lus", path, audio_dec_format_name(d->info.fmt),
                 d->info.samplerate, d->info.channels, d->info.bits,
                 (unsigned long)(d->info.duration_ms / 1000));
    }
    return d;
}

int audio_dec_read(audio_dec_t *d, int16_t *pcm, int max_frames)
{
    if (!d || !d->vt || !d->vt->read) {
        return -1;
    }
    return d->vt->read(d, pcm, max_frames);
}

bool audio_dec_seek_ms(audio_dec_t *d, uint32_t ms)
{
    if (!d || !d->vt || !d->vt->seek_ms) {
        return false;
    }
    return d->vt->seek_ms(d, ms);
}

uint32_t audio_dec_tell_ms(audio_dec_t *d)
{
    if (!d || !d->vt || !d->vt->tell_ms) {
        return 0;
    }
    return d->vt->tell_ms(d);
}

void audio_dec_close(audio_dec_t *d)
{
    if (!d) {
        return;
    }
    if (d->vt && d->vt->close) {
        d->vt->close(d);
    }
    free(d->impl);
    free(d);
}

const char *audio_dec_format_name(music_fmt_t fmt)
{
    switch (fmt) {
    case MUSIC_FMT_MP3:
        return "MP3";
    case MUSIC_FMT_FLAC:
        return "FLAC";
    case MUSIC_FMT_WAV:
        return "WAV";
    case MUSIC_FMT_AAC:
        return "AAC";
    case MUSIC_FMT_M4A:
        return "M4A";
    default:
        return "?";
    }
}
