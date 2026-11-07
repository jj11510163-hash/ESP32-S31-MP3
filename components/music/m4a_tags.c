#include "m4a_tags.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t type[4];
    long data;
    long end;
} box_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static bool read_box(FILE *file, long parent_end, box_t *box)
{
    long start = ftell(file);
    uint8_t head[16];
    if (start < 0 || parent_end - start < 8 || fread(head, 1, 8, file) != 8) return false;
    uint64_t size = be32(head);
    long header = 8;
    if (size == 1) {
        if (parent_end - start < 16 || fread(head + 8, 1, 8, file) != 8) return false;
        size = be64(head + 8);
        header = 16;
    } else if (size == 0) {
        size = (uint64_t)(parent_end - start);
    }
    if (size < (uint64_t)header || size > (uint64_t)(parent_end - start)) return false;
    memcpy(box->type, head + 4, 4);
    box->data = start + header;
    box->end = start + (long)size;
    return true;
}

static void copy_utf8(FILE *file, long bytes, char *dst, size_t capacity)
{
    if (!capacity) return;
    size_t count = bytes < (long)(capacity - 1) ? (size_t)bytes : capacity - 1;
    count = fread(dst, 1, count, file);
    size_t valid = 0;
    while (valid < count && dst[valid]) {
        uint8_t lead = (uint8_t)dst[valid];
        size_t width = lead < 0x80 ? 1 :
                       (lead & 0xE0) == 0xC0 ? 2 :
                       (lead & 0xF0) == 0xE0 ? 3 :
                       (lead & 0xF8) == 0xF0 ? 4 : 0;
        if (!width || valid + width > count) break;
        bool complete = true;
        for (size_t i = 1; i < width; i++) {
            if (((uint8_t)dst[valid + i] & 0xC0) != 0x80) complete = false;
        }
        if (!complete) break;
        valid += width;
    }
    dst[valid] = '\0';
}

static void parse_item(FILE *file, const box_t *item, m4a_tags_t *tags)
{
    char *text = NULL;
    size_t capacity = 0;
    if (memcmp(item->type, "\xA9nam", 4) == 0) {
        text = tags->title;
        capacity = sizeof(tags->title);
    } else if (memcmp(item->type, "\xA9" "ART", 4) == 0) {
        text = tags->artist;
        capacity = sizeof(tags->artist);
    } else if (memcmp(item->type, "aART", 4) == 0 && !tags->artist[0]) {
        text = tags->artist;
        capacity = sizeof(tags->artist);
    } else if (memcmp(item->type, "\xA9" "alb", 4) == 0) {
        text = tags->album;
        capacity = sizeof(tags->album);
    }
    while (ftell(file) >= 0 && ftell(file) < item->end) {
        box_t child;
        if (!read_box(file, item->end, &child)) break;
        if (memcmp(child.type, "data", 4) == 0 && child.end - child.data >= 8) {
            uint8_t data_head[8];
            if (fread(data_head, 1, 8, file) == 8) {
                uint32_t kind = be32(data_head) & 0xFFFFFF;
                long length = child.end - ftell(file);
                if (kind == 1 && text && length > 0) {
                    copy_utf8(file, length, text, capacity);
                } else if (memcmp(item->type, "covr", 4) == 0 &&
                           (kind == 13 || kind == 14) && length > 0 &&
                           !tags->cover_length) {
                    long offset = ftell(file);
                    if (offset >= 0 && (uint64_t)offset <= UINT32_MAX &&
                        (uint64_t)length <= UINT32_MAX) {
                        tags->cover_offset = (uint32_t)offset;
                        tags->cover_length = (uint32_t)length;
                    }
                } else if (kind == 1 && memcmp(item->type, "\xA9" "day", 4) == 0) {
                    char year[12] = {0};
                    copy_utf8(file, length, year, sizeof(year));
                    tags->year = (uint16_t)atoi(year);
                } else if (memcmp(item->type, "trkn", 4) == 0 && length >= 4) {
                    uint8_t track[4];
                    if (fread(track, 1, 4, file) == 4) {
                        tags->track_no = (uint16_t)(((uint16_t)track[2] << 8) | track[3]);
                    }
                }
            }
        }
        if (fseek(file, child.end, SEEK_SET) != 0) break;
    }
}

static void parse_mvhd(FILE *file, const box_t *box, m4a_tags_t *tags)
{
    uint8_t head[32];
    size_t count = (size_t)(box->end - box->data);
    if (count > sizeof(head)) count = sizeof(head);
    if (count < 20 || fread(head, 1, count, file) != count) return;
    uint32_t timescale;
    uint64_t duration;
    if (head[0] == 1 && count >= 32) {
        timescale = be32(head + 20);
        duration = be64(head + 24);
    } else if (head[0] == 0) {
        timescale = be32(head + 12);
        duration = be32(head + 16);
    } else return;
    if (timescale && duration != UINT64_MAX && duration != UINT32_MAX) {
        if (duration > (uint64_t)UINT32_MAX * timescale / 1000) {
            tags->duration_ms = UINT32_MAX;
            return;
        }
        uint64_t ms = duration * 1000 / timescale;
        tags->duration_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    }
}

static void walk_boxes(FILE *file, long end, int depth, int *remaining, m4a_tags_t *tags)
{
    while (*remaining > 0 && depth < 8 && ftell(file) >= 0 && ftell(file) < end) {
        box_t box;
        if (!read_box(file, end, &box)) break;
        (*remaining)--;
        if (memcmp(box.type, "mvhd", 4) == 0) {
            parse_mvhd(file, &box, tags);
        } else if (memcmp(box.type, "ilst", 4) == 0) {
            while (*remaining > 0 && ftell(file) >= 0 && ftell(file) < box.end) {
                box_t item;
                if (!read_box(file, box.end, &item)) break;
                (*remaining)--;
                parse_item(file, &item, tags);
                if (fseek(file, item.end, SEEK_SET) != 0) break;
            }
        } else if (memcmp(box.type, "moov", 4) == 0 ||
                   memcmp(box.type, "udta", 4) == 0) {
            if (box.end - ftell(file) >= 8) {
                walk_boxes(file, box.end, depth + 1, remaining, tags);
            }
        } else if (memcmp(box.type, "meta", 4) == 0) {
            if (box.end - ftell(file) >= 12 && fseek(file, 4, SEEK_CUR) == 0) {
                walk_boxes(file, box.end, depth + 1, remaining, tags);
            }
        }
        if (fseek(file, box.end, SEEK_SET) != 0) break;
    }
}

bool m4a_tags_read(const char *path, m4a_tags_t *tags)
{
    if (!path || !tags) return false;
    memset(tags, 0, sizeof(*tags));
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long end = ftell(file);
    if (end < 8 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    uint8_t head[8];
    bool valid = fread(head, 1, sizeof(head), file) == sizeof(head) &&
                 memcmp(head + 4, "ftyp", 4) == 0;
    if (valid && fseek(file, 0, SEEK_SET) == 0) {
        int remaining = 20000;
        walk_boxes(file, end, 0, &remaining, tags);
    }
    fclose(file);
    return valid;
}
