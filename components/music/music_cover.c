#include "music_cover.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "flac_tags.h"
#include "m4a_tags.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool syncsafe(const uint8_t *p, uint32_t *value)
{
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return false;
    *value = ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) |
             ((uint32_t)p[2] << 7) | p[3];
    return true;
}

static bool image_magic(FILE *file, long offset, long end)
{
    static const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    uint8_t head[8];
    if (offset < 0 || end - offset < (long)sizeof(head) ||
        fread(head, 1, sizeof(head), file) != sizeof(head)) return false;
    return (head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) ||
           memcmp(head, png, sizeof(png)) == 0;
}

static bool picture_data(FILE *file, long end, bool v22,
                         uint32_t *offset, uint32_t *length, int *type)
{
    int encoding = fgetc(file);
    if (encoding < 0 || encoding > 3) return false;
    if (v22) {
        if (end - ftell(file) < 3 || fseek(file, 3, SEEK_CUR) != 0) return false;
    } else {
        bool terminated = false;
        while (ftell(file) >= 0 && ftell(file) < end) {
            int ch = fgetc(file);
            if (ch == EOF) return false;
            if (ch == 0) { terminated = true; break; }
        }
        if (!terminated) return false;
    }
    if (ftell(file) >= end) return false;
    *type = fgetc(file);
    if (*type == EOF) return false;
    bool terminated = false;
    if (encoding == 0 || encoding == 3) {
        while (ftell(file) >= 0 && ftell(file) < end) {
            int ch = fgetc(file);
            if (ch == EOF) return false;
            if (ch == 0) { terminated = true; break; }
        }
    } else {
        while (ftell(file) >= 0 && end - ftell(file) >= 2) {
            int first = fgetc(file), second = fgetc(file);
            if (first == EOF || second == EOF) return false;
            if (first == 0 && second == 0) { terminated = true; break; }
        }
    }
    long data = ftell(file);
    if (!terminated || data < 0 || (uint64_t)data > UINT32_MAX ||
        end - data < 8 || (uint64_t)(end - data) > UINT32_MAX ||
        !image_magic(file, data, end)) return false;
    *offset = (uint32_t)data;
    *length = (uint32_t)(end - data);
    return true;
}

static bool mp3_cover_find(FILE *file, long file_size,
                           uint32_t *offset, uint32_t *length)
{
    uint8_t tag[10];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(tag, 1, sizeof(tag), file) != sizeof(tag) ||
        memcmp(tag, "ID3", 3) != 0 || tag[3] < 2 || tag[3] > 4 ||
        (tag[5] & 0x80)) return false;
    uint32_t tag_size;
    if (!syncsafe(tag + 6, &tag_size) || (uint64_t)tag_size + 10 > (uint64_t)file_size)
        return false;
    long end = 10 + (long)tag_size;
    if (tag[3] == 2 && (tag[5] & 0x40)) return false;
    if (tag[3] >= 3 && (tag[5] & 0x40)) {
        uint8_t ext[4];
        if (end - ftell(file) < 4 || fread(ext, 1, 4, file) != 4) return false;
        uint32_t size;
        if (tag[3] == 3) {
            size = be32(ext);
            if ((uint64_t)size + 4 > (uint64_t)(end - 10)) return false;
        } else {
            if (!syncsafe(ext, &size) || size < 4 || size > (uint32_t)(end - 10))
                return false;
            size -= 4;
        }
        if (fseek(file, size, SEEK_CUR) != 0) return false;
    }

    bool found = false;
    int selected_type = -1;
    for (int frames = 0; frames < 4096; frames++) {
        long frame_start = ftell(file);
        int header_size = tag[3] == 2 ? 6 : 10;
        if (frame_start < 0 || end - frame_start < header_size) break;
        uint8_t frame[10];
        if (fread(frame, 1, header_size, file) != (size_t)header_size || frame[0] == 0)
            break;
        int id_size = tag[3] == 2 ? 3 : 4;
        bool valid_id = true;
        for (int i = 0; i < id_size; i++) {
            if (!((frame[i] >= 'A' && frame[i] <= 'Z') ||
                  (frame[i] >= '0' && frame[i] <= '9'))) valid_id = false;
        }
        if (!valid_id) break;
        uint32_t size = tag[3] == 2 ?
            ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 8) | frame[5] :
            tag[3] == 3 ? be32(frame + 4) : 0;
        if (tag[3] == 4 && !syncsafe(frame + 4, &size)) break;
        long data = ftell(file);
        if (data < 0 || size > (uint32_t)(end - data)) break;
        long frame_end = data + (long)size;
        bool apic = tag[3] == 2 ? memcmp(frame, "PIC", 3) == 0 :
                                   memcmp(frame, "APIC", 4) == 0;
        if (apic && tag[3] == 3 && (frame[9] & 0xe0)) apic = false;
        if (apic && tag[3] == 4) {
            if (frame[9] & 0x0e) apic = false;
            if (apic && (frame[9] & 0x40)) {
                if (data == frame_end) apic = false;
                else if (fseek(file, 1, SEEK_CUR) != 0) apic = false;
                else data++;
            }
            if (apic && (frame[9] & 0x01)) {
                if (frame_end - data < 4) apic = false;
                else if (fseek(file, 4, SEEK_CUR) != 0) apic = false;
                else data += 4;
            }
        }
        if (apic && size && selected_type != 3) {
            uint32_t image_offset, image_length;
            int type;
            if (picture_data(file, frame_end, tag[3] == 2,
                             &image_offset, &image_length, &type) &&
                (!found || type == 3)) {
                *offset = image_offset;
                *length = image_length;
                selected_type = type;
                found = true;
            }
        }
        if (found && selected_type == 3) break;
        if (fseek(file, frame_end, SEEK_SET) != 0) break;
    }
    return found;
}

bool music_cover_find(const char *path, uint32_t *offset, uint32_t *length)
{
    if (!path || !offset || !length) return false;
    *offset = 0;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_size = ftell(file);
    uint8_t head[12];
    bool valid = file_size >= (long)sizeof(head) &&
                 fseek(file, 0, SEEK_SET) == 0 &&
                 fread(head, 1, sizeof(head), file) == sizeof(head);
    bool mp3 = valid && memcmp(head, "ID3", 3) == 0;
    bool flac = valid && memcmp(head, "fLaC", 4) == 0;
    bool m4a = valid && memcmp(head + 4, "ftyp", 4) == 0;
    bool found = mp3 && mp3_cover_find(file, file_size, offset, length);
    fclose(file);
    if (flac) {
        flac_tags_t tags;
        if (flac_tags_read(path, &tags) && tags.cover_length) {
            *offset = tags.cover_offset;
            *length = tags.cover_length;
            found = true;
        }
    } else if (m4a) {
        m4a_tags_t tags;
        if (m4a_tags_read(path, &tags) && tags.cover_length) {
            *offset = tags.cover_offset;
            *length = tags.cover_length;
            found = true;
        }
    }
    return found;
}
