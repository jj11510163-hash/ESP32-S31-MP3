/* dec_flac.c —— FLAC 后端（dr_flac，纯 C，支持 16/24bit 与高采样率） */
#include <stdio.h>
#include <string.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "decoder.h"
#include "esp_log.h"

typedef struct {
    drflac *f;
    unsigned initial_empty_reads;
} flac_impl_t;

static int flac_read(audio_dec_t *ad, int16_t *pcm, int max_frames)
{
    flac_impl_t *im = ad->impl;
    if (im->f == NULL) {
        return -1;
    }
    drflac_uint64 n;
    if (ad->info.channels == 2) {
        n = drflac_read_pcm_frames_s16(im->f, (drflac_uint64)max_frames, pcm);
    } else {
        /* 单声道 FLAC：先解成单声道再复制成立体声 */
        int16_t *tmp = pcm;
        n = drflac_read_pcm_frames_s16(im->f, (drflac_uint64)max_frames, tmp);
        for (int i = (int)n - 1; i >= 0; i--) {
            pcm[i * 2 + 1] = tmp[i];
            pcm[i * 2] = tmp[i];
        }
    }
    if (n == 0 && im->f->currentPCMFrame == 0 && im->initial_empty_reads++ < 4) {
        FILE *file = (FILE *)im->f->bs.pUserData;
        ESP_LOGW("flac", "initial read empty: first_frame=%llu, declared_frames=%llu, cursor=%ld, eof=%d, io_error=%d",
                 (unsigned long long)im->f->firstFLACFramePosInBytes,
                 (unsigned long long)im->f->totalPCMFrameCount,
                 ftell(file), feof(file), ferror(file));
        if (ferror(file)) clearerr(file);
    }
    return (int)n;
}

static bool flac_seek(audio_dec_t *ad, uint32_t ms)
{
    flac_impl_t *im = ad->impl;
    if (!im->f) {
        return false;
    }
    drflac_uint64 frame = (drflac_uint64)ms * (drflac_uint64)im->f->sampleRate / 1000ULL;
    if (frame > im->f->totalPCMFrameCount) {
        frame = im->f->totalPCMFrameCount;
    }
    return drflac_seek_to_pcm_frame(im->f, frame) == DRFLAC_TRUE;
}

static uint32_t flac_tell(audio_dec_t *ad)
{
    flac_impl_t *im = ad->impl;
    if (!im->f || im->f->sampleRate == 0) {
        return 0;
    }
    return (uint32_t)(im->f->currentPCMFrame * 1000ULL / (drflac_uint64)im->f->sampleRate);
}

static void flac_free(audio_dec_t *ad)
{
    flac_impl_t *im = ad->impl;
    if (im->f) {
        drflac_close(im->f);
        im->f = NULL;
    }
}

static const audio_decoder_vtbl_t s_vt = {
    .read = flac_read,
    .seek_ms = flac_seek,
    .tell_ms = flac_tell,
    .close = flac_free,
};

audio_dec_t *dec_flac_open(const char *path)
{
    drflac *f = drflac_open_file(path, NULL);
    if (!f) {
        ESP_LOGW("flac", "open failed: %s", path);
        return NULL;
    }
    if (f->channels < 1 || f->channels > 2) {
        ESP_LOGW("flac", "unsupported channel count %u", f->channels);
        drflac_close(f);
        return NULL;
    }
    void *impl = NULL;
    audio_dec_t *d = audio_dec_alloc(&s_vt, sizeof(flac_impl_t), &impl);
    if (!d) {
        drflac_close(f);
        return NULL;
    }
    ((flac_impl_t *)impl)->f = f;
    d->info.samplerate = (int)f->sampleRate;
    d->info.channels = (int)f->channels;
    d->info.bits = (int)f->bitsPerSample;
    d->info.duration_ms = (f->sampleRate && f->totalPCMFrameCount)
                              ? (uint32_t)(f->totalPCMFrameCount * 1000ULL / f->sampleRate)
                              : 0;
    d->info.fmt = MUSIC_FMT_FLAC;
    return d;
}
