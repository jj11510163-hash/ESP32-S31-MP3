/* Native test of the production WAV decoder against files supplied on the command line. */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bsp.h"
#include "decoder.h"
#include "ff.h"

static const wchar_t *s_input_path;
static FSIZE_t s_last_seek;

bool bsp_sd_fat_path(const char *vfs_path, char *out, size_t out_size)
{
    if (strcmp(vfs_path, "/sdcard/test.wav") != 0) return false;
    int count = snprintf(out, out_size, "0:/test.wav");
    return count >= 0 && (size_t)count < out_size;
}

int f_open(FIL *file, const char *path, unsigned char mode)
{
    (void)path;
    if (mode != FA_READ) return 1;
    file->fp = _wfopen(s_input_path, L"rb");
    if (!file->fp) return 1;
    if (_fseeki64(file->fp, 0, SEEK_END) != 0) {
        fclose(file->fp);
        return 1;
    }
    __int64 size = _ftelli64(file->fp);
    if (size < 0 || size > UINT32_MAX || _fseeki64(file->fp, 0, SEEK_SET) != 0) {
        fclose(file->fp);
        return 1;
    }
    file->size = (FSIZE_t)size;
    return FR_OK;
}

int f_close(FIL *file)
{
    int result = fclose(file->fp);
    file->fp = NULL;
    return result == 0 ? FR_OK : 1;
}

int f_read(FIL *file, void *dst, UINT bytes, UINT *read_bytes)
{
    *read_bytes = (UINT)fread(dst, 1, bytes, file->fp);
    return ferror(file->fp) ? 1 : FR_OK;
}

int f_lseek(FIL *file, FSIZE_t offset)
{
    if (_fseeki64(file->fp, offset, SEEK_SET) != 0) return 1;
    s_last_seek = offset;
    return FR_OK;
}

FSIZE_t f_tell(FIL *file)
{
    return (FSIZE_t)_ftelli64(file->fp);
}

FSIZE_t f_size(FIL *file)
{
    return file->size;
}

audio_dec_t *audio_dec_alloc(const audio_decoder_vtbl_t *vt, size_t impl_size, void **impl_out)
{
    audio_dec_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->impl = calloc(1, impl_size);
    if (!decoder->impl) {
        free(decoder);
        return NULL;
    }
    decoder->vt = vt;
    *impl_out = decoder->impl;
    return decoder;
}

static void close_decoder(audio_dec_t *decoder)
{
    decoder->vt->close(decoder);
    free(decoder->impl);
    free(decoder);
}

static bool test_file(const wchar_t *path, int index, uint32_t expected_ms,
                      int expected_rate, int expected_bits, int expected_channels,
                      bool test_large_seek)
{
    s_input_path = path;
    audio_dec_t *decoder = dec_wav_open("/sdcard/test.wav");
    if (!decoder) {
        fprintf(stderr, "file %d: open failed\n", index);
        return false;
    }

    bool okay = decoder->info.fmt == MUSIC_FMT_WAV &&
                decoder->info.channels == expected_channels &&
                decoder->info.samplerate == expected_rate &&
                decoder->info.bits == expected_bits &&
                decoder->info.duration_ms == expected_ms;
    int16_t pcm[256 * 2];
    if (okay) okay = decoder->vt->read(decoder, pcm, 256) == 256;

    uint32_t middle_ms = expected_ms / 2;
    if (okay) okay = decoder->vt->seek_ms(decoder, middle_ms);
    if (okay) okay = decoder->vt->tell_ms(decoder) >= middle_ms - 1;
    if (okay) okay = decoder->vt->read(decoder, pcm, 256) == 256;

    if (test_large_seek && okay) {
        uint32_t late_ms = expected_ms * 3 / 4;
        okay = decoder->vt->seek_ms(decoder, late_ms) && s_last_seek > INT32_MAX;
        if (okay) okay = decoder->vt->read(decoder, pcm, 256) == 256;
    }

    if (okay) okay = decoder->vt->seek_ms(decoder, expected_ms - 10);
    int tail_frames = 0;
    if (okay) {
        for (int i = 0; i < 100; ++i) {
            int got = decoder->vt->read(decoder, pcm, 256);
            if (got < 0) { okay = false; break; }
            if (got == 0) break;
            tail_frames += got;
        }
        okay = okay && tail_frames > 0 && tail_frames < expected_rate / 2;
    }

    if (okay) okay = decoder->vt->seek_ms(decoder, expected_ms + 1000) &&
                     decoder->vt->read(decoder, pcm, 256) == 0;

    printf("file %d: %s (%u ms, tail %d frames)\n", index, okay ? "PASS" : "FAIL",
           decoder->info.duration_ms, tail_frames);
    close_decoder(decoder);
    return okay;
}

static bool parse_u32(const wchar_t *value, uint32_t *out)
{
    if (*value == L'-') return false;
    wchar_t *end;
    unsigned long parsed = wcstoul(value, &end, 10);
    if (value == end || *end != L'\0' || parsed > UINT32_MAX) return false;
    *out = (uint32_t)parsed;
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 7 || (argc - 1) % 6 != 0) {
        fprintf(stderr, "usage: wav_host_test <file> <duration_ms> <sample_rate> "
                        "<bits> <channels> <check_seek_past_2gb:0|1> [...]\n");
        return 2;
    }

    bool okay = true;
    for (int i = 1; i < argc; i += 6) {
        uint32_t duration, rate, bits, channels, large_seek;
        if (!parse_u32(argv[i + 1], &duration) ||
            !parse_u32(argv[i + 2], &rate) ||
            !parse_u32(argv[i + 3], &bits) ||
            !parse_u32(argv[i + 4], &channels) ||
            !parse_u32(argv[i + 5], &large_seek) || large_seek > 1 ||
            duration < 20 || rate == 0 || rate > INT32_MAX ||
            (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
            (channels != 1 && channels != 2)) {
            fprintf(stderr, "file %d: invalid test parameters\n", (i - 1) / 6 + 1);
            return 2;
        }
        if (!test_file(argv[i], (i - 1) / 6 + 1, duration, (int)rate,
                       (int)bits, (int)channels, large_seek != 0))
            okay = false;
    }
    return okay ? 0 : 1;
}
