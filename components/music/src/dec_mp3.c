/* dec_mp3.c —— helix MP3 后端（把 mp3_dec.c 包成统一接口） */
#include "decoder.h"
#include "mp3_dec.h"

typedef struct {
    mp3_dec_t *d;
} mp3_impl_t;

static int mp3_read(audio_dec_t *ad, int16_t *pcm, int max_frames)
{
    mp3_impl_t *im = ad->impl;
    if (im->d == NULL) {
        return -1;
    }
    if (max_frames > 1152) {
        max_frames = 1152; /* helix 一帧最多 1152 每声道 */
    }
    int n = mp3_next(im->d, pcm, max_frames);
    if (n <= 0) {
        return n;
    }
    if (ad->info.channels == 1) {
        for (int i = n - 1; i >= 0; i--) {
            pcm[i * 2 + 1] = pcm[i];
            pcm[i * 2] = pcm[i];
        }
    }
    return n;
}

static bool mp3_seek(audio_dec_t *ad, uint32_t ms)
{
    mp3_impl_t *im = ad->impl;
    return im->d ? mp3_seek_ms(im->d, ms) : false;
}

static uint32_t mp3_tell(audio_dec_t *ad)
{
    mp3_impl_t *im = ad->impl;
    return im->d ? mp3_tell_ms(im->d) : 0;
}

static void mp3_free(audio_dec_t *ad)
{
    mp3_impl_t *im = ad->impl;
    if (im->d) {
        mp3_close(im->d);
        im->d = NULL;
    }
}

static const audio_decoder_vtbl_t s_vt = {
    .read = mp3_read,
    .seek_ms = mp3_seek,
    .tell_ms = mp3_tell,
    .close = mp3_free,
};

audio_dec_t *dec_mp3_open(const char *path)
{
    mp3_info_t info;
    mp3_dec_t *m = mp3_open(path, &info);
    if (!m) {
        return NULL;
    }
    void *impl = NULL;
    audio_dec_t *d = audio_dec_alloc(&s_vt, sizeof(mp3_impl_t), &impl);
    if (!d) {
        mp3_close(m);
        return NULL;
    }
    ((mp3_impl_t *)impl)->d = m;
    d->info.samplerate = info.samplerate;
    d->info.channels = info.channels;
    d->info.bits = 16;
    d->info.duration_ms = mp3_duration_ms(m);
    d->info.fmt = MUSIC_FMT_MP3;
    return d;
}
