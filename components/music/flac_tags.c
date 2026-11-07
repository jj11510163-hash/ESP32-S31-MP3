#include "flac_tags.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_u32(FILE *file, bool little_endian, uint32_t *value)
{
    unsigned char b[4];
    if (fread(b, 1, 4, file) != 4) return false;
    *value = little_endian ?
        (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24) :
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

static bool bytes_left(FILE *file, long end, long needed)
{
    long pos = ftell(file);
    return pos >= 0 && pos <= end && end - pos >= needed;
}

static bool key_is(const char *key, const char *expected)
{
    while (*key && *expected) {
        char a = *key++, b = *expected++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return false;
    }
    return *key == 0 && *expected == 0;
}

static void read_text(FILE *file, uint32_t length, char *out, size_t capacity)
{
    size_t n = length < capacity - 1 ? length : capacity - 1;
    n = fread(out, 1, n, file);
    /* Avoid ending a truncated value in the middle of a UTF-8 character. */
    if (n && n < length) {
        size_t start = n - 1;
        while (start && ((unsigned char)out[start] & 0xC0) == 0x80) start--;
        unsigned char lead = (unsigned char)out[start];
        size_t bytes = (lead & 0xF8) == 0xF0 ? 4 :
                       (lead & 0xF0) == 0xE0 ? 3 :
                       (lead & 0xE0) == 0xC0 ? 2 : 1;
        if (n - start < bytes) n = start;
    }
    out[n] = '\0';
}

static void parse_streaminfo(FILE *file, long end, flac_tags_t *tags)
{
    unsigned char info[34];
    if (!bytes_left(file, end, sizeof(info)) ||
        fread(info, 1, sizeof(info), file) != sizeof(info)) return;
    uint64_t packed = 0;
    for (int i = 10; i < 18; i++) packed = (packed << 8) | info[i];
    tags->sample_rate = (uint32_t)(packed >> 44);
    tags->channels = (uint8_t)(((packed >> 41) & 7) + 1);
    tags->bits = (uint8_t)(((packed >> 36) & 31) + 1);
    tags->total_samples = packed & ((1ULL << 36) - 1);
}

static void parse_comments(FILE *file, long end, flac_tags_t *tags)
{
    uint32_t vendor, count;
    if (!bytes_left(file, end, 4) || !read_u32(file, true, &vendor) ||
        vendor > (uint32_t)(end - ftell(file)) ||
        fseek(file, (long)vendor, SEEK_CUR) != 0 || !bytes_left(file, end, 4) ||
        !read_u32(file, true, &count)) return;
    for (uint32_t i = 0; i < count && i < 4096; i++) {
        uint32_t length;
        if (!bytes_left(file, end, 4) || !read_u32(file, true, &length)) return;
        long start = ftell(file);
        if (start < 0 || start > end || length > (uint32_t)(end - start)) return;
        long item_end = start + (long)length;
        char key[32];
        size_t key_len = 0;
        bool separator = false;
        while (ftell(file) < item_end && key_len < sizeof(key) - 1) {
            int c = fgetc(file);
            if (c == '=') { separator = true; break; }
            if (c == EOF) return;
            key[key_len++] = (char)c;
        }
        key[key_len] = '\0';
        if (separator) {
            long value_offset = ftell(file);
            uint32_t value_length = (uint32_t)(item_end - value_offset);
            if (key_is(key, "TITLE")) read_text(file, value_length, tags->title, sizeof(tags->title));
            else if (key_is(key, "ARTIST")) read_text(file, value_length, tags->artist, sizeof(tags->artist));
            else if (key_is(key, "ALBUM")) read_text(file, value_length, tags->album, sizeof(tags->album));
            else if ((key_is(key, "PUBLISHER") || key_is(key, "LABEL") ||
                      key_is(key, "ORGANIZATION")) && !tags->publisher[0]) {
                read_text(file, value_length, tags->publisher, sizeof(tags->publisher));
            }
            else if (key_is(key, "LYRICS") && value_offset >= 0 &&
                     (uint64_t)value_offset <= UINT32_MAX) {
                tags->lyrics_offset = (uint32_t)value_offset;
                tags->lyrics_length = value_length;
            } else if (key_is(key, "DATE") || key_is(key, "YEAR")) {
                char number[16];
                read_text(file, value_length, number, sizeof(number));
                tags->year = (uint16_t)strtoul(number, NULL, 10);
            } else if (key_is(key, "TRACKNUMBER")) {
                char number[16];
                read_text(file, value_length, number, sizeof(number));
                tags->track_no = (uint16_t)strtoul(number, NULL, 10);
            }
        }
        if (fseek(file, item_end, SEEK_SET) != 0) return;
    }
}

static void parse_picture(FILE *file, long end, flac_tags_t *tags)
{
    uint32_t type, mime_length, description_length, unused, image_length;
    if (!bytes_left(file, end, 8) || !read_u32(file, false, &type) ||
        !read_u32(file, false, &mime_length) ||
        mime_length > (uint32_t)(end - ftell(file)) ||
        fseek(file, (long)mime_length, SEEK_CUR) != 0) return;
    if (!bytes_left(file, end, 4) || !read_u32(file, false, &description_length) ||
        description_length > (uint32_t)(end - ftell(file)) ||
        fseek(file, (long)description_length, SEEK_CUR) != 0) return;
    if (!bytes_left(file, end, 20)) return;
    for (int i = 0; i < 4; i++) {
        if (!read_u32(file, false, &unused)) return;
    }
    if (!read_u32(file, false, &image_length)) return;
    long offset = ftell(file);
    if (offset < 0 || (uint64_t)offset > UINT32_MAX ||
        image_length > (uint32_t)(end - offset)) return;
    unsigned char magic[8];
    bool has_magic = image_length >= sizeof(magic) &&
                     fread(magic, 1, sizeof(magic), file) == sizeof(magic);
    bool jpeg = has_magic && magic[0] == 0xff && magic[1] == 0xd8 &&
                magic[2] == 0xff;
    bool png = has_magic && memcmp(magic, "\x89PNG\r\n\x1a\n", 8) == 0;
    if ((jpeg || png) && (!tags->cover_length || type == 3)) {
        tags->cover_offset = (uint32_t)offset;
        tags->cover_length = image_length;
    }
    if (jpeg && (!tags->jpeg_length || type == 3)) {
        tags->jpeg_offset = (uint32_t)offset;
        tags->jpeg_length = image_length;
    }
}

bool flac_tags_read(const char *path, flac_tags_t *tags)
{
    if (!path || !tags) return false;
    memset(tags, 0, sizeof(*tags));
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_size = ftell(file);
    if (file_size < 4 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    unsigned char magic[4];
    bool ok = fread(magic, 1, 4, file) == 4 && memcmp(magic, "fLaC", 4) == 0;
    bool saw_last = false;
    for (int blocks = 0; ok && blocks < 128; blocks++) {
        unsigned char head[4];
        if (fread(head, 1, 4, file) != 4) { ok = false; break; }
        long start = ftell(file);
        uint32_t length = ((uint32_t)head[1] << 16) | ((uint32_t)head[2] << 8) | head[3];
        if (start < 0 || (uint64_t)length > (uint64_t)LONG_MAX - (uint64_t)start) {
            ok = false;
            break;
        }
        long end = start + (long)length;
        if (end > file_size) { ok = false; break; }
        if ((head[0] & 0x7F) == 0) parse_streaminfo(file, end, tags);
        else if ((head[0] & 0x7F) == 4) parse_comments(file, end, tags);
        else if ((head[0] & 0x7F) == 6) parse_picture(file, end, tags);
        if (fseek(file, end, SEEK_SET) != 0) { ok = false; break; }
        if (head[0] & 0x80) { saw_last = true; break; }
    }
    fclose(file);
    return ok && saw_last;
}
