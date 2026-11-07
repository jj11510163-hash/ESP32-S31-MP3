#include "mp3_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mp3dec.h"

static const char *TAG = "mp3dec";

#define READ_BUF_SIZE (8 * 1024)
#define MAX_SAMPLES_PER_CH 1152

struct mp3_dec {
    FILE *fp;
    HMP3Decoder heli;
    mp3_info_t info;

    uint8_t *rbuf;
    int rlen;   /* rbuf 中剩余有效字节 */
    uint8_t *cursor;

    uint32_t frames;
    uint32_t total_frames_est;
};

/* 跳过 ID3v2 头，返回音频起始偏移 */
static int32_t skip_id3v2(FILE *fp)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return 0;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        fseek(fp, 0, SEEK_SET);
        return 0;
    }
    int32_t size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
    int32_t total = 10 + size;
    if (hdr[5] & 0x10) {
        total += 10; /* footer */
    }
    return total;
}

mp3_dec_t *mp3_open(const char *path, mp3_info_t *out_info)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    mp3_dec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(fp);
        return NULL;
    }
    d->rbuf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d->rbuf) {
        free(d);
        fclose(fp);
        return NULL;
    }
    d->fp = fp;
    d->heli = MP3InitDecoder();
    if (!d->heli) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        free(d->rbuf);
        free(d);
        fclose(fp);
        return NULL;
    }

    d->info.audio_offset = skip_id3v2(fp);
    d->info.file_size = (int32_t)fsize;
    fseek(fp, d->info.audio_offset, SEEK_SET);

    /* 预读并解一帧，拿到采样率/声道/码率 */
    d->rlen = (int)fread(d->rbuf, 1, READ_BUF_SIZE, fp);
    d->cursor = d->rbuf;
    if (d->rlen <= 0) {
        free(d->rbuf);
        free(d);
        fclose(fp);
        return NULL;
    }
    int off = MP3FindSyncWord(d->rbuf, d->rlen);
    if (off < 0) {
        ESP_LOGW(TAG, "no mp3 sync word");
        free(d->rbuf);
        free(d);
        fclose(fp);
        return NULL;
    }
    d->cursor += off;
    d->rlen -= off;
    uint8_t *probe = d->cursor;
    int probe_len = d->rlen;
    short *pcm = heap_caps_malloc(MAX_SAMPLES_PER_CH * 2 * sizeof(short), MALLOC_CAP_SPIRAM);
    int err = MP3Decode(d->heli, &probe, &probe_len, pcm, 0);
    if (err == 0) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d->heli, &fi);
        d->info.samplerate = fi.samprate;
        d->info.channels = fi.nChans;
        d->info.bitrate = fi.bitrate / 1000;
        d->info.frame_samples = fi.outputSamps / (fi.nChans ? fi.nChans : 1);
    } else {
        d->info.samplerate = 44100;
        d->info.channels = 2;
        d->info.bitrate = 128;
        d->info.frame_samples = MAX_SAMPLES_PER_CH;
    }
    free(pcm);

    /* 回到起点重新开始 */
    fseek(fp, d->info.audio_offset, SEEK_SET);
    d->rlen = (int)fread(d->rbuf, 1, READ_BUF_SIZE, fp);
    d->cursor = d->rbuf;
    d->frames = 0;

    int32_t audio_bytes = d->info.file_size - d->info.audio_offset;
    if (d->info.bitrate > 0) {
        d->total_frames_est = (uint32_t)((int64_t)audio_bytes * 8 / (d->info.bitrate * 1000));
    }

    ESP_LOGI(TAG, "%s: %dHz %dch %dkbps %lus", path, d->info.samplerate, d->info.channels,
             d->info.bitrate, (unsigned long)mp3_duration_ms(d));

    if (out_info) {
        *out_info = d->info;
    }
    return d;
}

static int refill(mp3_dec_t *d)
{
    if (d->cursor > d->rbuf) {
        memmove(d->rbuf, d->cursor, (size_t)d->rlen);
        d->cursor = d->rbuf;
    }
    int space = READ_BUF_SIZE - d->rlen;
    if (space <= 0) {
        return 0;
    }
    int n = (int)fread(d->rbuf + d->rlen, 1, (size_t)space, d->fp);
    if (n > 0) {
        d->rlen += n;
    }
    return n;
}

int mp3_next(mp3_dec_t *d, int16_t *pcm, int max_samples)
{
    if (d == NULL) {
        return -1;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        if (d->rlen < 4) {
            if (refill(d) <= 0) {
                return 0;
            }
        }

        int off = MP3FindSyncWord(d->cursor, d->rlen);
        if (off < 0) {
            /* 没找到同步头：丢掉前面数据继续找 */
            d->cursor += d->rlen;
            d->rlen = 0;
            if (refill(d) <= 0) {
                return 0;
            }
            continue;
        }
        if (off > 0) {
            d->cursor += off;
            d->rlen -= off;
        }

        uint8_t *frame_start = d->cursor;
        int frame_len = d->rlen;
        uint8_t *in = frame_start;
        int in_len = frame_len;
        int err = MP3Decode(d->heli, &in, &in_len, pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            /* Helix advances past the header before detecting a short frame. */
            if (frame_len == READ_BUF_SIZE) {
                ESP_LOGW(TAG, "MP3 frame exceeds input buffer");
                return -1;
            }
            if (refill(d) <= 0) {
                return 0;
            }
            continue;
        }
        int consumed = frame_len - in_len;
        if (consumed > 0) {
            d->cursor += consumed;
            d->rlen -= consumed;
        }
        if (err != 0) {
            /* 损坏帧：跳过 1 字节重新同步 */
            if (consumed <= 0 && d->rlen > 1) {
                d->cursor++;
                d->rlen--;
            }
            continue;
        }

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d->heli, &fi);
        int per_ch = fi.outputSamps / (fi.nChans ? fi.nChans : 1);
        if (per_ch > max_samples) {
            per_ch = max_samples;
        }
        d->frames++;
        return per_ch;
    }
    return -1;
}

uint32_t mp3_duration_ms(const mp3_dec_t *d)
{
    if (!d || d->info.bitrate <= 0) {
        return 0;
    }
    int32_t audio_bytes = d->info.file_size - d->info.audio_offset;
    if (audio_bytes <= 0) {
        return 0;
    }
    return (uint32_t)(((int64_t)audio_bytes * 8) / d->info.bitrate);
}

uint32_t mp3_tell_ms(mp3_dec_t *d)
{
    if (!d || d->info.bitrate <= 0) {
        return 0;
    }
    long pos = ftell(d->fp) - d->rlen;
    long rel = pos - d->info.audio_offset;
    if (rel < 0) {
        rel = 0;
    }
    return (uint32_t)(((int64_t)rel * 8) / d->info.bitrate);
}

bool mp3_seek_ms(mp3_dec_t *d, uint32_t ms)
{
    if (!d || d->info.bitrate <= 0) {
        return false;
    }
    int64_t byte_pos = (int64_t)ms * d->info.bitrate / 8;
    long target = d->info.audio_offset + (long)byte_pos;
    if (fseek(d->fp, target, SEEK_SET) != 0) {
        return false;
    }
    d->rlen = 0;
    d->cursor = d->rbuf;
    return refill(d) > 0;
}

void mp3_close(mp3_dec_t *d)
{
    if (!d) {
        return;
    }
    if (d->heli) {
        MP3FreeDecoder(d->heli);
    }
    if (d->fp) {
        fclose(d->fp);
    }
    free(d->rbuf);
    free(d);
}

bool mp3_probe(const char *path, uint32_t *dur_ms, uint16_t *bitrate, uint16_t *srate, uint8_t *channels)
{
    mp3_info_t info;
    mp3_dec_t *d = mp3_open(path, &info);
    if (!d) {
        return false;
    }
    if (dur_ms) {
        *dur_ms = mp3_duration_ms(d);
    }
    if (bitrate) {
        *bitrate = (uint16_t)info.bitrate;
    }
    if (srate) {
        *srate = (uint16_t)info.samplerate;
    }
    if (channels) {
        *channels = (uint8_t)info.channels;
    }
    mp3_close(d);
    return true;
}
