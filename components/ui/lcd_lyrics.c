#include "lcd_lyrics.h"
#include "flac_tags.h"

#include <stdio.h>
#include <string.h>

#define MAX_LINES 320
#define LINE_BYTES 96

typedef struct {
    uint32_t ms;
    char text[LINE_BYTES];
} lyric_line_t;

static lyric_line_t s_lines[MAX_LINES];
static int s_count;

static bool read_row(FILE *file, char row[256], uint32_t *remaining, bool bounded)
{
    size_t used = 0;
    bool got = false;
    while (!bounded || *remaining) {
        int c = fgetc(file);
        if (c == EOF) break;
        if (bounded) (*remaining)--;
        got = true;
        if (c == '\n') break;
        if (used < 255) row[used++] = (char)c;
    }
    row[used] = '\0';
    return got;
}

static size_t inline_han_start(const char *text, size_t length)
{
    bool latin = false;
    for (size_t i = 0; i < length; i++) {
        const unsigned char *p = (const unsigned char *)text + i;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) latin = true;
        if (latin && i + 2 < length && (p[0] & 0xF0) == 0xE0 &&
            (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                          ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp >= 0x3400 && cp <= 0x9FFF) return i;
        }
    }
    return length;
}

static void add_line(uint32_t ms, const char *text, size_t length)
{
    if (!length || s_count >= MAX_LINES) return;
    if (length >= LINE_BYTES) {
        length = LINE_BYTES - 1;
        while (length && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
    }
    lyric_line_t *line = &s_lines[s_count++];
    line->ms = ms;
    memcpy(line->text, text, length);
    line->text[length] = '\0';
}

static const char *parse_time(const char *p, uint32_t *ms)
{
    if (*p++ != '[') return NULL;
    if (*p < '0' || *p > '9') return NULL;
    unsigned min = 0, sec = 0, frac = 0, frac_digits = 0;
    while (*p >= '0' && *p <= '9') { min = min * 10 + (*p++ - '0'); if (min > 999) return NULL; }
    if (*p++ != ':') return NULL;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') { sec = sec * 10 + (*p++ - '0'); if (sec > 59) return NULL; }
    if (*p == '.' || *p == ':') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (frac_digits < 3) { frac = frac * 10 + (*p - '0'); frac_digits++; }
            p++;
        }
    }
    if (*p++ != ']') return NULL;
    if (frac_digits == 1) frac *= 100;
    else if (frac_digits == 2) frac *= 10;
    *ms = (min * 60 + sec) * 1000 + frac;
    return p;
}

void lcd_lyrics_load(const char *track_path)
{
    s_count = 0;
    if (!track_path || !track_path[0]) return;
    char path[300];
    size_t length = strlen(track_path);
    if (length + 5 >= sizeof(path)) return;
    memcpy(path, track_path, length + 1);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) dot = path + length;
    memcpy(dot, ".lrc", 5);
    FILE *file = fopen(path, "rb");
    uint32_t remaining = 0;
    bool bounded = false;
    if (!file) {
        flac_tags_t tags;
        if (!flac_tags_read(track_path, &tags) || !tags.lyrics_length) return;
        file = fopen(track_path, "rb");
        if (!file || fseek(file, (long)tags.lyrics_offset, SEEK_SET) != 0) {
            if (file) fclose(file);
            return;
        }
        remaining = tags.lyrics_length;
        bounded = true;
    }
    char row[256];
    int32_t offset_ms = 0;
    while (s_count < MAX_LINES && read_row(file, row, &remaining, bounded)) {
        const char *p = row;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) p += 3;
        if (strncmp(p, "[offset:", 8) == 0) {
            int value;
            if (sscanf(p + 8, "%d", &value) == 1) offset_ms = value;
            continue;
        }
        if (remaining == 0 && bounded) {
            size_t used = strlen(row);
            if (used && (unsigned char)row[used - 1] >= 0x80) {
                size_t start = used - 1;
                while (start && ((unsigned char)row[start] & 0xC0) == 0x80) start--;
                unsigned char lead = (unsigned char)row[start];
                size_t bytes = (lead & 0xF8) == 0xF0 ? 4 :
                               (lead & 0xF0) == 0xE0 ? 3 :
                               (lead & 0xE0) == 0xC0 ? 2 : 1;
                if (used - start < bytes) row[start] = '\0';
            }
        }
        uint32_t times[8];
        int n_times = 0;
        while (n_times < 8) {
            uint32_t ms;
            const char *next = parse_time(p, &ms);
            if (!next) break;
            times[n_times++] = ms;
            p = next;
        }
        while (*p == ' ' || *p == '\t') p++;
        size_t text_len = strcspn(p, "\r\n");
        if (!n_times || !text_len) continue;
        size_t split = inline_han_start(p, text_len);
        size_t original_len = split;
        while (original_len && (p[original_len - 1] == ' ' || p[original_len - 1] == '\t'))
            original_len--;
        for (int i = 0; i < n_times && s_count < MAX_LINES; i++) {
            if (split < text_len && original_len) {
                add_line(times[i], p, original_len);
                add_line(times[i], p + split, text_len - split);
            } else {
                add_line(times[i], p, text_len);
            }
        }
    }
    fclose(file);
    if (offset_ms) {
        for (int i = 0; i < s_count; i++) {
            int64_t adjusted = (int64_t)s_lines[i].ms + offset_ms;
            s_lines[i].ms = adjusted < 0 ? 0 : (uint32_t)adjusted;
        }
    }
    /* Some LRC tools repeat timestamps; keep lines in time order. */
    for (int i = 1; i < s_count; i++) {
        lyric_line_t item = s_lines[i];
        int j = i - 1;
        while (j >= 0 && s_lines[j].ms > item.ms) {
            s_lines[j + 1] = s_lines[j];
            j--;
        }
        s_lines[j + 1] = item;
    }
}

bool lcd_lyrics_available(void)
{
    return s_count > 0;
}

static int group_end(int start)
{
    int end = start + 1;
    while (end < s_count && s_lines[end].ms == s_lines[start].ms) end++;
    return end;
}

static bool contains_latin(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) return true;
    }
    return false;
}

static bool contains_han(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2] &&
            (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                          ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp >= 0x3400 && cp <= 0x9FFF) return true;
            p += 3;
        } else {
            p++;
        }
    }
    return false;
}

static const char *group_primary(int start, const char **translation)
{
    static const char empty[] = "";
    if (translation) *translation = empty;
    if (start < 0) return empty;

    int latin = -1, han = -1;
    for (int i = start, end = group_end(start); i < end; i++) {
        if (han < 0 && contains_han(s_lines[i].text)) han = i;
        else if (latin < 0 && contains_latin(s_lines[i].text)) latin = i;
    }
    if (latin >= 0 && han >= 0) {
        if (translation) *translation = s_lines[han].text;
        return s_lines[latin].text;
    }
    return s_lines[start].text;
}

void lcd_lyrics_at(uint32_t pos_ms, const char **previous,
                   const char **current, const char **translation,
                   const char **next)
{
    int previous_start = -1, active_start = -1, next_start = -1;
    for (int i = 0; i < s_count;) {
        if (s_lines[i].ms > pos_ms) {
            next_start = i;
            break;
        }
        previous_start = active_start;
        active_start = i;
        i = group_end(i);
    }
    *previous = group_primary(previous_start, NULL);
    *current = group_primary(active_start, translation);
    *next = group_primary(next_start, NULL);
}
