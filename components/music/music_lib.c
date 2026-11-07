#include "music.h"
#include "flac_tags.h"
#include "m4a_tags.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_core.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "decoder.h"

static const char *TAG = "music_lib";

#define MAX_ITEMS 400
#define LIB_CACHE_PATH BSP_SD_MOUNT_POINT "/.s31_library_cache"
#define LIB_CACHE_TEMP BSP_SD_MOUNT_POINT "/.s31_library_cache_tmp"
#define LIB_CACHE_VERSION 1

typedef struct {
    uint32_t count;
    uint64_t sum;
    uint64_t mix;
    bool complete;
} library_fingerprint_t;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t item_size;
    uint32_t count;
    uint32_t file_count;
    uint32_t scope;
    uint64_t file_sum;
    uint64_t file_mix;
    uint32_t checksum;
    char app_version[32];
} library_cache_header_t;

static const char s_cache_magic[8] = {'S', '3', '1', 'L', 'I', 'B', '1', 0};

static music_item_t *s_items;
static int s_count;
static uint32_t s_generation;
static volatile bool s_scanning;
static bool s_scan_pending;
static volatile music_sd_owner_t s_sd_owner;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

/* Forward declaration: the text-frame decoder needs to distinguish UTF-8
 * from legacy single/DBCS metadata before choosing a conversion path. */
static bool utf8_valid(const char *s);

/* ------------------------------------------------------------------ */
/* ID3 解析                                                            */
/* ------------------------------------------------------------------ */
static size_t append_utf8(uint32_t cp, char *dst, size_t pos, size_t dst_size)
{
    size_t need;
    if (cp < 0x80) {
        need = 1;
    } else if (cp < 0x800) {
        need = 2;
    } else if (cp < 0x10000) {
        need = 3;
    } else if (cp <= 0x10FFFF) {
        need = 4;
    } else {
        cp = 0xFFFD;
        need = 3;
    }
    if (pos + need >= dst_size) {
        return pos;
    }
    if (need == 1) {
        dst[pos++] = (char)cp;
    } else if (need == 2) {
        dst[pos++] = (char)(0xC0 | (cp >> 6));
        dst[pos++] = (char)(0x80 | (cp & 0x3F));
    } else if (need == 3) {
        dst[pos++] = (char)(0xE0 | (cp >> 12));
        dst[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[pos++] = (char)(0x80 | (cp & 0x3F));
    } else {
        dst[pos++] = (char)(0xF0 | (cp >> 18));
        dst[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[pos++] = (char)(0x80 | (cp & 0x3F));
    }
    return pos;
}

static void utf16_to_utf8(const uint8_t *src, int len, char *dst, size_t dst_size, bool big_endian)
{
    size_t o = 0;
    if (dst_size == 0) {
        return;
    }
    if (len % 2) {
        len--;
    }
    for (int i = 0; i + 1 < len;) {
        uint32_t cp = big_endian ? (uint32_t)((src[i] << 8) | src[i + 1])
                                 : (uint32_t)((src[i + 1] << 8) | src[i]);
        i += 2;
        if (cp == 0) {
            break;
        }
        /* Convert a UTF-16 surrogate pair to one Unicode code point. */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t low = big_endian ? (uint32_t)((src[i] << 8) | src[i + 1])
                                      : (uint32_t)((src[i + 1] << 8) | src[i]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        size_t next = append_utf8(cp, dst, o, dst_size);
        if (next == o) {
            break;
        }
        o = next;
    }
    dst[o] = '\0';
}

static size_t utf8_char_len(const uint8_t *p, size_t remaining)
{
    if (remaining == 0 || p[0] == 0) {
        return 0;
    }
    if (p[0] < 0x80) {
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && remaining >= 2 && (p[1] & 0xC0) == 0x80) {
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && remaining >= 3 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80) {
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && remaining >= 4 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        return 4;
    }
    /* Keep malformed bytes so utf8_valid() can reject the complete field. */
    return 1;
}

static bool utf8_valid_bytes(const uint8_t *src, size_t len)
{
    size_t i = 0;
    while (i < len && src[i] != 0) {
        uint8_t b = src[i];
        if (b < 0x80) {
            i++;
            continue;
        }

        size_t n;
        if ((b & 0xE0) == 0xC0) {
            n = 2;
            /* Reject overlong two-byte encodings. */
            if (b < 0xC2) {
                return false;
            }
        } else if ((b & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4;
            if (b > 0xF4) {
                return false;
            }
        } else {
            return false;
        }
        if (i + n > len) {
            return false;
        }
        for (size_t j = 1; j < n; j++) {
            if (src[i + j] == 0 || (src[i + j] & 0xC0) != 0x80) {
                return false;
            }
        }
        /* Reject UTF-16 surrogate code points and overlong 3/4-byte forms. */
        if ((n == 3 && b == 0xE0 && src[i + 1] < 0xA0) ||
            (n == 3 && b == 0xED && src[i + 1] >= 0xA0) ||
            (n == 4 && b == 0xF0 && src[i + 1] < 0x90) ||
            (n == 4 && b == 0xF4 && src[i + 1] >= 0x90)) {
            return false;
        }
        i += n;
    }
    return true;
}

static void copy_bounded_text(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    size_t o = 0;
    if (dst_size == 0) {
        return;
    }
    while (o < len && src[o] != 0) {
        size_t n = utf8_char_len(src + o, len - o);
        if (n == 0 || o + n >= dst_size) {
            break;
        }
        memcpy(dst + o, src + o, n);
        o += n;
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\0')) {
        o--;
    }
    dst[o] = '\0';
}

static bool is_gbk_lead(uint8_t b)
{
    return b >= 0x81 && b <= 0xFE;
}

static bool is_gbk_trail(uint8_t b)
{
    return b >= 0x40 && b <= 0xFE && b != 0x7F;
}

/* Convert legacy GBK/CP936 metadata using the same table as FatFs.  This
 * avoids a second 20K+ mapping table in the application and keeps Chinese
 * filenames and tags on the same Unicode basis. */
static void gbk_to_utf8(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    size_t o = 0;
    if (dst_size == 0) {
        return;
    }

    for (size_t i = 0; i < len && src[i] != 0;) {
        uint8_t b = src[i++];
        uint32_t cp = 0;

        if (b < 0x80) {
            cp = b;
        } else if (is_gbk_lead(b) && i < len && src[i] != 0 && is_gbk_trail(src[i])) {
            cp = ff_oem2uni((WCHAR)(((uint16_t)b << 8) | src[i]), 936);
            i++;
        } else {
            /* ISO-8859-1 tags occasionally contain an isolated high byte.
             * Keep it readable instead of treating it as a malformed UTF-8
             * sequence.  CP936's 0x80 Euro sign is handled explicitly. */
            cp = ff_oem2uni((WCHAR)b, 936);
            if (cp == 0) {
                cp = (b == 0x80) ? 0x20AC : b;
            }
        }

        if (cp == 0) {
            cp = 0xFFFD;
        }
        size_t next = append_utf8(cp, dst, o, dst_size);
        if (next == o) {
            break;
        }
        o = next;
    }

    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = '\0';
}

static void copy_legacy_text(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    size_t n = 0;
    while (n < len && src[n] != 0) {
        n++;
    }
    if (utf8_valid_bytes(src, n)) {
        copy_bounded_text(src, n, dst, dst_size);
    } else {
        gbk_to_utf8(src, n, dst, dst_size);
    }
}

static void copy_text_frame(const uint8_t *data, int size, char *dst, size_t dst_size)
{
    if (dst_size > 0) {
        dst[0] = '\0';
    }
    if (size <= 1 || dst_size == 0) {
        return;
    }
    uint8_t enc = data[0];
    const uint8_t *p = data + 1;
    int len = size - 1;

    /* A few taggers prepend a UTF-8 BOM even though ID3 enc=3 does not need
     * one.  It is metadata, not part of the visible title. */
    if (enc == 0x03 && len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
        len -= 3;
    }

    if (enc == 0x01 || enc == 0x02) {
        /* ID3 enc=1 is UTF-16 and should carry a BOM.  A number of taggers
         * omit it anyway and write little-endian data, so use LE as the
         * practical default; enc=2 explicitly means big-endian. */
        bool be = (enc == 0x02);
        if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
            be = false;
            p += 2;
            len -= 2;
        } else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
            p += 2;
            len -= 2;
        }
        utf16_to_utf8(p, len, dst, dst_size, be);
        return;
    }

    /* ID3 enc=0 is nominally ISO-8859-1, but Chinese tagging tools commonly
     * store raw GBK here.  Preserve valid UTF-8 and convert everything else
     * through CP936 before the final metadata validation. */
    if ((enc == 0x00 || enc == 0x03) && !utf8_valid_bytes(p, (size_t)len)) {
        gbk_to_utf8(p, (size_t)len, dst, dst_size);
    } else {
        copy_bounded_text(p, (size_t)len, dst, dst_size);
    }
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t id3_frame_size(const uint8_t *p, uint8_t version)
{
    return version >= 4 ? syncsafe32(p) : be32(p);
}

static void parse_id3v2(FILE *fp, music_item_t *it)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        return;
    }
    uint32_t size = syncsafe32(&hdr[6]);
    if (size < 10 || size > 512 * 1024) {
        return;
    }
    uint8_t *buf = malloc(size);
    if (!buf) {
        return;
    }
    size_t got = fread(buf, 1, size, fp);
    if (got < 10) {
        free(buf);
        return;
    }

    uint8_t version = hdr[3];
    uint32_t pos = 0;
    if (hdr[5] & 0x40) { /* extended header */
        if (got >= 4) {
            uint32_t ext_size = version >= 4 ? syncsafe32(buf) : be32(buf);
            /* v2.3 excludes the four-byte size field; v2.4 includes it. */
            pos = version >= 4 ? ext_size : 4 + ext_size;
        }
    }

    while (pos + 10 <= got) {
        const uint8_t *f = buf + pos;
        if (f[0] == 0) {
            break;
        }
        char id[5] = {(char)f[0], (char)f[1], (char)f[2], (char)f[3], 0};
        uint32_t fsize = id3_frame_size(f + 4, version);
        if (fsize == 0 || pos + 10 + fsize > got) {
            break;
        }
        const uint8_t *data = f + 10;
        if (strcmp(id, "TIT2") == 0) {
            copy_text_frame(data, (int)fsize, it->title, sizeof(it->title));
        } else if (strcmp(id, "TPE1") == 0) {
            copy_text_frame(data, (int)fsize, it->artist, sizeof(it->artist));
        } else if (strcmp(id, "TALB") == 0) {
            copy_text_frame(data, (int)fsize, it->album, sizeof(it->album));
        } else if (strcmp(id, "TPUB") == 0) {
            copy_text_frame(data, (int)fsize, it->publisher, sizeof(it->publisher));
        } else if (strcmp(id, "TRCK") == 0) {
            char tmp[16] = {0};
            copy_text_frame(data, (int)fsize, tmp, sizeof(tmp));
            it->track_no = (uint16_t)atoi(tmp);
        } else if (strcmp(id, "TYER") == 0 || strcmp(id, "TDRC") == 0) {
            char tmp[16] = {0};
            copy_text_frame(data, (int)fsize, tmp, sizeof(tmp));
            it->year = (uint16_t)atoi(tmp);
        }
        pos += 10 + fsize;
    }
    free(buf);
}

static void parse_id3v1(FILE *fp, music_item_t *it)
{
    if (fseek(fp, -128, SEEK_END) != 0) {
        return;
    }
    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), fp) != sizeof(tag)) {
        return;
    }
    if (memcmp(tag, "TAG", 3) != 0) {
        return;
    }
    if (it->title[0] == '\0') {
        copy_legacy_text(tag + 3, 30, it->title, sizeof(it->title));
    }
    if (it->artist[0] == '\0') {
        copy_legacy_text(tag + 33, 30, it->artist, sizeof(it->artist));
    }
    if (it->album[0] == '\0') {
        copy_legacy_text(tag + 63, 30, it->album, sizeof(it->album));
    }
    if (it->year == 0) {
        char y[5] = {0};
        memcpy(y, tag + 93, 4);
        it->year = (uint16_t)atoi(y);
    }
}

/* 判断字符串是否是合法 UTF-8（用于过滤 GBK 乱码的 ID3 文本） */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int n;
        if ((*p & 0xE0) == 0xC0) {
            n = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            n = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            n = 3;
        } else {
            return false;
        }
        for (int i = 1; i <= n; i++) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        p += n + 1;
    }
    return true;
}

music_fmt_t music_fmt_from_path(const char *path)
{
    if (!path) {
        return MUSIC_FMT_UNKNOWN;
    }
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return MUSIC_FMT_UNKNOWN;
    }
    if (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".mp2") == 0) {
        return MUSIC_FMT_MP3;
    }
    if (strcasecmp(dot, ".flac") == 0) {
        return MUSIC_FMT_FLAC;
    }
    if (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".wave") == 0) {
        return MUSIC_FMT_WAV;
    }
    if (strcasecmp(dot, ".aac") == 0) {
        return MUSIC_FMT_AAC;
    }
    if (strcasecmp(dot, ".m4a") == 0) {
        return MUSIC_FMT_M4A;
    }
    return MUSIC_FMT_UNKNOWN;
}

const char *music_fmt_name(music_fmt_t fmt)
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
        return "--";
    }
}

static void basename_no_ext(const char *path, char *out, size_t out_size)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void load_metadata(music_item_t *it)
{
    if (it->fmt == MUSIC_FMT_FLAC) {
        flac_tags_t tags;
        if (flac_tags_read(it->path, &tags)) {
            memcpy(it->title, tags.title, sizeof(it->title));
            memcpy(it->artist, tags.artist, sizeof(it->artist));
            memcpy(it->album, tags.album, sizeof(it->album));
            memcpy(it->publisher, tags.publisher, sizeof(it->publisher));
            it->year = tags.year;
            it->track_no = tags.track_no;
        }
    } else if (it->fmt == MUSIC_FMT_M4A) {
        m4a_tags_t tags;
        if (m4a_tags_read(it->path, &tags)) {
            memcpy(it->title, tags.title, sizeof(it->title));
            memcpy(it->artist, tags.artist, sizeof(it->artist));
            memcpy(it->album, tags.album, sizeof(it->album));
            it->year = tags.year;
            it->track_no = tags.track_no;
            it->duration_ms = tags.duration_ms;
        }
    } else if (it->fmt == MUSIC_FMT_MP3 || it->fmt == MUSIC_FMT_AAC) {
        FILE *fp = fopen(it->path, "rb");
        if (!fp) return;
        parse_id3v2(fp, it);
        if (it->title[0] == '\0' || it->artist[0] == '\0') {
            parse_id3v1(fp, it);
        }
        fclose(fp);
    }

    /* 有些中文工具把 GBK 塞进 ID3 的 "ISO-8859-1" 字段，转不出来的直接丢弃，
       后面会退回用文件名（FAT 长文件名经 VFS 转换后一定是合法 UTF-8）。 */
    if (!utf8_valid(it->title)) {
        it->title[0] = '\0';
    }
    if (!utf8_valid(it->artist)) {
        it->artist[0] = '\0';
    }
    if (!utf8_valid(it->album)) {
        it->album[0] = '\0';
    }
    if (!utf8_valid(it->publisher)) {
        it->publisher[0] = '\0';
    }

    /* 去掉 ID3v1 常见的尾部空格 */
    for (int i = (int)strlen(it->title) - 1; i >= 0 && it->title[i] == ' '; i--) {
        it->title[i] = '\0';
    }
    if (it->title[0] == '\0') {
        basename_no_ext(it->path, it->title, sizeof(it->title));
    }

    audio_dec_t *dec = audio_dec_open(it->path, it->fmt);
    if (dec) {
        if (dec->info.duration_ms) it->duration_ms = dec->info.duration_ms;
        it->samplerate = (uint32_t)dec->info.samplerate;
        it->bits = (uint8_t)dec->info.bits;
        it->channels = (uint8_t)dec->info.channels;
        audio_dec_close(dec);
    }
}

static int cmp_items(const void *a, const void *b)
{
    const music_item_t *x = a;
    const music_item_t *y = b;
    if (x->track_no && y->track_no && x->track_no != y->track_no) {
        return (int)x->track_no - (int)y->track_no;
    }
    return strcasecmp(x->path, y->path);
}

static uint64_t fingerprint_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void fingerprint_add(library_fingerprint_t *fp, const char *path,
                            uint64_t size, int64_t modified)
{
    uint64_t hash = fingerprint_bytes(14695981039346656037ULL, path, strlen(path));
    hash = fingerprint_bytes(hash, &size, sizeof(size));
    hash = fingerprint_bytes(hash, &modified, sizeof(modified));
    fp->count++;
    fp->sum += hash;
    fp->mix ^= hash;
}

static void walk_dir(const char *dir, int depth, bool load_items, library_fingerprint_t *fp)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (errno != ENOENT) fp->complete = false;
        return;
    }
    struct dirent *e;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) {
            if (errno != 0) fp->complete = false;
            break;
        }
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[MUSIC_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof(path)) {
            continue;
        }
        struct stat st;
        bool have_stat = stat(path, &st) == 0;
        if (have_stat && S_ISDIR(st.st_mode)) {
            if (depth < 3) {
                walk_dir(path, depth + 1, load_items, fp);
            }
            continue;
        }
        music_fmt_t fmt = music_fmt_from_path(e->d_name);
        if (fmt == MUSIC_FMT_UNKNOWN) {
            if (!have_stat) fp->complete = false;
            continue;
        }

        uint64_t file_size = have_stat ? (uint64_t)st.st_size : 0;
        int64_t modified = have_stat ? (int64_t)st.st_mtime : 0;
        if (fmt == MUSIC_FMT_WAV && (!have_stat || st.st_size < 0)) {
            char fat_path[MUSIC_PATH_MAX + 4];
            FILINFO info;
            if (bsp_sd_fat_path(path, fat_path, sizeof(fat_path)) &&
                f_stat(fat_path, &info) == FR_OK && !(info.fattrib & AM_DIR)) {
                file_size = (uint64_t)info.fsize;
                if (!have_stat) modified = ((int64_t)info.fdate << 16) | info.ftime;
            } else if (have_stat) {
                file_size = (uint32_t)st.st_size;
            } else {
                fp->complete = false;
                continue;
            }
        } else if (!have_stat) {
            fp->complete = false;
            continue;
        }

        fingerprint_add(fp, path, file_size, modified);
        if (!load_items || s_count >= MAX_ITEMS) continue;

        music_item_t *it = &s_items[s_count];
        memset(it, 0, sizeof(*it));
        snprintf(it->path, sizeof(it->path), "%s", path);
        it->size = (uint32_t)file_size;
        it->fmt = fmt;
        load_metadata(it);
        s_count++;
        if ((s_count % 8) == 0) {
            app_evt_t evt = {.id = APP_EVT_LIB_PROGRESS};
            evt.lib.count = s_count;
            app_evt_post(&evt, 0);
            vTaskDelay(1);
        }
    }
    closedir(d);
}

static library_fingerprint_t library_walk(bool load_items, uint32_t *scope)
{
    library_fingerprint_t fp = {.complete = true};
    *scope = 0;
    walk_dir(MUSIC_SCAN_DIR, 0, load_items, &fp);
    if (fp.count == 0) {
        ESP_LOGW(TAG, "no files in %s, fallback to /sdcard", MUSIC_SCAN_DIR);
        bool complete = fp.complete;
        fp = (library_fingerprint_t){.complete = true};
        *scope = 1;
        walk_dir(BSP_SD_MOUNT_POINT, 0, load_items, &fp);
        fp.complete = fp.complete && complete;
    }
    return fp;
}

static uint32_t cache_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static bool cache_load(void)
{
    FILE *file = fopen(LIB_CACHE_PATH, "rb");
    if (!file) return false;
    library_cache_header_t header;
    bool valid = fread(&header, sizeof(header), 1, file) == 1 &&
                 memcmp(header.magic, s_cache_magic, sizeof(s_cache_magic)) == 0 &&
                 header.version == LIB_CACHE_VERSION &&
                 header.item_size == sizeof(music_item_t) &&
                 header.count <= MAX_ITEMS &&
                 strncmp(header.app_version, esp_app_get_description()->version,
                         sizeof(header.app_version)) == 0;
    if (valid) {
        uint32_t scope;
        library_fingerprint_t fp = library_walk(false, &scope);
        valid = fp.complete && header.file_count == fp.count &&
                header.count == (fp.count < MAX_ITEMS ? fp.count : MAX_ITEMS) &&
                header.scope == scope &&
                header.file_sum == fp.sum && header.file_mix == fp.mix;
    }
    if (valid) {
        size_t bytes = (size_t)header.count * sizeof(music_item_t);
        valid = fread(s_items, 1, bytes, file) == bytes &&
                cache_checksum(s_items, bytes) == header.checksum &&
                fgetc(file) == EOF && !ferror(file);
        for (uint32_t i = 0; valid && i < header.count; ++i) {
            const music_item_t *it = &s_items[i];
            valid = memchr(it->path, 0, sizeof(it->path)) != NULL &&
                    memchr(it->title, 0, sizeof(it->title)) != NULL &&
                    memchr(it->artist, 0, sizeof(it->artist)) != NULL &&
                    memchr(it->album, 0, sizeof(it->album)) != NULL &&
                    memchr(it->publisher, 0, sizeof(it->publisher)) != NULL &&
                    strncmp(it->path, BSP_SD_MOUNT_POINT "/", sizeof(BSP_SD_MOUNT_POINT)) == 0 &&
                    it->fmt == music_fmt_from_path(it->path);
        }
    }
    fclose(file);
    if (valid) s_count = (int)header.count;
    return valid;
}

static void cache_save(const library_fingerprint_t *fp, uint32_t scope)
{
    if (!fp->complete || s_count != (fp->count < MAX_ITEMS ? fp->count : MAX_ITEMS)) return;
    size_t bytes = (size_t)s_count * sizeof(music_item_t);
    library_cache_header_t header = {
        .version = LIB_CACHE_VERSION,
        .item_size = sizeof(music_item_t),
        .count = (uint32_t)s_count,
        .file_count = fp->count,
        .scope = scope,
        .file_sum = fp->sum,
        .file_mix = fp->mix,
        .checksum = cache_checksum(s_items, bytes),
    };
    memcpy(header.magic, s_cache_magic, sizeof(s_cache_magic));
    snprintf(header.app_version, sizeof(header.app_version), "%s",
             esp_app_get_description()->version);
    FILE *file = fopen(LIB_CACHE_TEMP, "wb");
    if (!file) return;
    bool good = fwrite(&header, sizeof(header), 1, file) == 1 &&
                fwrite(s_items, 1, bytes, file) == bytes &&
                fflush(file) == 0;
    if (fclose(file) != 0) good = false;
    if (good) {
        if (remove(LIB_CACHE_PATH) != 0 && errno != ENOENT) good = false;
        if (good && rename(LIB_CACHE_TEMP, LIB_CACHE_PATH) != 0) good = false;
    }
    if (!good) {
        remove(LIB_CACHE_TEMP);
        ESP_LOGW(TAG, "could not save library cache");
    } else {
        ESP_LOGI(TAG, "library cache saved: %d tracks", s_count);
    }
}

static void scan_task(void *arg)
{
    bool allow_cache = (uintptr_t)arg != 0;
    if (!s_items) {
        s_items = heap_caps_calloc(MAX_ITEMS, sizeof(music_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_items) {
            s_items = calloc(MAX_ITEMS, sizeof(music_item_t));
        }
    }
    if (!s_items) {
        ESP_LOGE(TAG, "no memory for library");
        portENTER_CRITICAL(&s_scan_mux);
        s_scanning = false;
        s_scan_pending = false;
        portEXIT_CRITICAL(&s_scan_mux);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        s_count = 0;
        bool from_cache = allow_cache && cache_load();
        allow_cache = false;
        if (from_cache) {
            ESP_LOGI(TAG, "library cache hit: %d tracks", s_count);
        } else {
            ESP_LOGI(TAG, "scanning %s ...", MUSIC_SCAN_DIR);
            uint32_t scope;
            library_fingerprint_t fp = library_walk(true, &scope);
            if (s_count > 1) {
                qsort(s_items, (size_t)s_count, sizeof(music_item_t), cmp_items);
            }
            cache_save(&fp, scope);
        }

        esp_err_t favorites_err = music_favorites_refresh();
        if (favorites_err != ESP_OK) {
            ESP_LOGW(TAG, "favorites unavailable: %s", esp_err_to_name(favorites_err));
        }

        portENTER_CRITICAL(&s_scan_mux);
        bool again = s_scan_pending;
        s_scan_pending = false;
        if (!again) {
            __atomic_add_fetch(&s_generation, 1, __ATOMIC_RELEASE);
            s_scanning = false;
        }
        portEXIT_CRITICAL(&s_scan_mux);
        if (again) {
            continue;
        }

        ESP_LOGI(TAG, "library ready: %d tracks", s_count);
        app_evt_t evt = {.id = APP_EVT_LIB_READY};
        evt.lib.count = s_count;
        app_evt_post(&evt, 0);
        break;
    }

    vTaskDelete(NULL);
}

esp_err_t music_lib_init(void)
{
    s_count = 0;
    s_generation = 0;
    s_items = NULL;
    if (!dec_aac_init()) {
        ESP_LOGW(TAG, "AAC decoder unavailable");
    }
    return music_favorites_init();
}

static esp_err_t start_scan(bool allow_cache)
{
    portENTER_CRITICAL(&s_scan_mux);
    if (s_sd_owner != MUSIC_SD_OWNER_NONE) {
        portEXIT_CRITICAL(&s_scan_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        s_scan_pending = true;
        portEXIT_CRITICAL(&s_scan_mux);
        return ESP_OK;
    }
    s_scanning = true;
    portEXIT_CRITICAL(&s_scan_mux);

    if (xTaskCreate(scan_task, "scan", 8192, (void *)(uintptr_t)allow_cache, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_scan_mux);
        s_scanning = false;
        s_scan_pending = false;
        portEXIT_CRITICAL(&s_scan_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t music_lib_scan(void)
{
    return start_scan(true);
}

esp_err_t music_lib_rescan_async(void)
{
    return start_scan(false);
}

int music_lib_count(void)
{
    return s_count;
}

const music_item_t *music_lib_get(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return &s_items[index];
}

bool music_lib_scanning(void)
{
    return s_scanning;
}

bool music_sd_try_acquire(music_sd_owner_t owner)
{
    if (owner == MUSIC_SD_OWNER_NONE) return false;
    portENTER_CRITICAL(&s_scan_mux);
    bool acquired = s_sd_owner == MUSIC_SD_OWNER_NONE;
    if (acquired) {
        s_sd_owner = owner;
        bsp_sd_set_exclusive(true);
    }
    portEXIT_CRITICAL(&s_scan_mux);
    return acquired;
}

void music_sd_release(music_sd_owner_t owner)
{
    if (owner == MUSIC_SD_OWNER_NONE) return;
    portENTER_CRITICAL(&s_scan_mux);
    if (s_sd_owner == owner) {
        bsp_sd_set_exclusive(false);
        s_sd_owner = MUSIC_SD_OWNER_NONE;
    }
    portEXIT_CRITICAL(&s_scan_mux);
}

music_sd_owner_t music_sd_owner(void)
{
    return __atomic_load_n(&s_sd_owner, __ATOMIC_ACQUIRE);
}

bool music_update_mode(void)
{
    return music_sd_owner() != MUSIC_SD_OWNER_NONE;
}

uint32_t music_lib_generation(void)
{
    return __atomic_load_n(&s_generation, __ATOMIC_ACQUIRE);
}
