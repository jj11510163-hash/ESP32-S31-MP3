/* WAV: PCM 8/16/24/32-bit and float32, mono/stereo, including extensible. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "decoder.h"
#include "esp_log.h"
#include "ff.h"

typedef struct {
    FIL file;
    uint64_t data_off;
    uint64_t data_size;
    uint64_t pos;
    uint32_t block_align;
    uint32_t samplerate;
    int fmt_tag;
    int bits;
    int channels;
    uint8_t *raw;
    uint32_t raw_cap;
} wav_impl_t;

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool read_exact(FIL *file, void *dst, UINT size)
{
    UINT count = 0;
    return f_read(file, dst, size, &count) == FR_OK && count == size;
}

static bool seek_abs(FIL *file, uint64_t offset)
{
    if (offset > (uint64_t)((FSIZE_t)-1)) return false;
    return f_lseek(file, (FSIZE_t)offset) == FR_OK;
}

static bool parse_header(wav_impl_t *w, audio_info_t *info, const char *path)
{
    uint8_t hdr[12];
    if (!read_exact(&w->file, hdr, sizeof(hdr)) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;

    const uint64_t file_size = f_size(&w->file);
    bool got_fmt = false;
    bool got_data = false;
    for (int guard = 0; guard < 64 && (uint64_t)f_tell(&w->file) + 8 <= file_size; ++guard) {
        uint8_t chunk[8];
        if (!read_exact(&w->file, chunk, sizeof(chunk))) return false;
        uint32_t size = rd32le(chunk + 4);
        uint64_t start = f_tell(&w->file);
        uint64_t next = start + (uint64_t)size + (size & 1U);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || start + size > file_size) return false;
            uint8_t fmt[40] = {0};
            UINT want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (!read_exact(&w->file, fmt, want)) return false;
            w->fmt_tag = rd16le(fmt);
            w->channels = rd16le(fmt + 2);
            w->samplerate = rd32le(fmt + 4);
            w->block_align = rd16le(fmt + 12);
            w->bits = rd16le(fmt + 14);
            if (w->fmt_tag == 0xFFFE) {
                static const uint8_t guid_tail[14] = {
                    0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71
                };
                if (size < 40 || rd16le(fmt + 16) < 22 ||
                    memcmp(fmt + 26, guid_tail, sizeof(guid_tail)) != 0)
                    return false;
                w->fmt_tag = rd16le(fmt + 24);
            }
            got_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!got_fmt) return false;
            w->data_off = start;
            uint64_t available = file_size - start;
            w->data_size = size < available ? size : available;
            if ((uint64_t)size > available)
                ESP_LOGW("wav", "truncated data chunk: %s (%llu of %lu bytes)", path,
                         (unsigned long long)available, (unsigned long)size);
            got_data = true;
            break;
        }

        if (next > file_size || !seek_abs(&w->file, next)) return false;
    }

    if (!got_fmt || !got_data || (w->fmt_tag != 1 && w->fmt_tag != 3) ||
        w->channels < 1 || w->channels > 2 || w->samplerate == 0 ||
        w->samplerate > INT_MAX ||
        (w->fmt_tag == 3 && w->bits != 32) ||
        (w->fmt_tag == 1 && w->bits != 8 && w->bits != 16 &&
         w->bits != 24 && w->bits != 32) ||
        w->block_align != (uint32_t)w->channels * (uint32_t)(w->bits / 8))
        return false;

    w->data_size -= w->data_size % w->block_align;
    uint64_t duration = (w->data_size / w->block_align) * 1000ULL / w->samplerate;
    info->samplerate = (int)w->samplerate;
    info->channels = w->channels;
    info->bits = w->bits;
    info->fmt = MUSIC_FMT_WAV;
    info->duration_ms = duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
    return seek_abs(&w->file, w->data_off);
}

static int wav_read(audio_dec_t *ad, int16_t *pcm, int max_frames)
{
    wav_impl_t *w = ad->impl;
    if (w->pos >= w->data_size || max_frames <= 0) return 0;

    uint64_t available = (w->data_size - w->pos) / w->block_align;
    uint32_t frames = (uint32_t)max_frames;
    if (frames > available) frames = (uint32_t)available;
    uint32_t limit = w->raw_cap / w->block_align;
    if (frames > limit) frames = limit;
    if (frames == 0) return -1;
    UINT bytes = frames * w->block_align;
    if (!read_exact(&w->file, w->raw, bytes)) return -1;
    w->pos += bytes;

    const int bps = w->bits / 8;
    const uint8_t *p = w->raw;
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t s[2] = {0, 0};
        for (int c = 0; c < w->channels; ++c) {
            int16_t v;
            if (w->fmt_tag == 3) {
                float f;
                memcpy(&f, p, sizeof(f));
                if (f != f) f = 0.0f;
                f = f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
                v = (int16_t)(f * 32767.0f);
            } else if (bps == 1) {
                v = (int16_t)(((int)p[0] - 128) << 8);
            } else if (bps == 2) {
                v = (int16_t)rd16le(p);
            } else if (bps == 3) {
                int32_t x = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
                if (x & 0x800000) x -= 0x1000000;
                v = (int16_t)(x >> 8);
            } else {
                v = (int16_t)((int32_t)rd32le(p) >> 16);
            }
            s[c] = v;
            p += bps;
        }
        pcm[i * 2] = s[0];
        pcm[i * 2 + 1] = w->channels == 2 ? s[1] : s[0];
    }
    return (int)frames;
}

static bool wav_seek(audio_dec_t *ad, uint32_t ms)
{
    wav_impl_t *w = ad->impl;
    uint64_t frame = (uint64_t)ms * w->samplerate / 1000ULL;
    uint64_t off = frame * w->block_align;
    if (off > w->data_size) off = w->data_size;
    if (!seek_abs(&w->file, w->data_off + off)) return false;
    w->pos = off;
    return true;
}

static uint32_t wav_tell(audio_dec_t *ad)
{
    wav_impl_t *w = ad->impl;
    uint64_t ms = (w->pos / w->block_align) * 1000ULL / w->samplerate;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void wav_free(audio_dec_t *ad)
{
    wav_impl_t *w = ad->impl;
    f_close(&w->file);
    free(w->raw);
}

static const audio_decoder_vtbl_t s_vt = {
    .read = wav_read,
    .seek_ms = wav_seek,
    .tell_ms = wav_tell,
    .close = wav_free,
};

audio_dec_t *dec_wav_open(const char *path)
{
    char fat_path[MUSIC_PATH_MAX];
    if (!bsp_sd_fat_path(path, fat_path, sizeof(fat_path))) return NULL;

    void *impl = NULL;
    audio_dec_t *d = audio_dec_alloc(&s_vt, sizeof(wav_impl_t), &impl);
    if (!d) return NULL;
    wav_impl_t *w = impl;
    if (f_open(&w->file, fat_path, FA_READ) != FR_OK) goto fail_alloc;
    if (!parse_header(w, &d->info, path)) {
        ESP_LOGW("wav", "unsupported wav: %s", path);
        goto fail_file;
    }
    w->raw_cap = 8192;
    w->raw = malloc(w->raw_cap);
    if (!w->raw) goto fail_file;
    return d;

fail_file:
    f_close(&w->file);
fail_alloc:
    free(w->raw);
    free(w);
    free(d);
    return NULL;
}
