#include "ui_cjk.h"

#include <string.h>

#include "esp_log.h"

/*
 * 二进制格式（小端）：
 *   0  "S31F"
 *   4  u8  width
 *   5  u8  height
 *   6  u16 stride
 *   8  u32 count
 *   12 entries[count] { u32 codepoint; u32 offset; }  按码位升序
 *   .. 位图数据
 */
typedef struct {
    const uint8_t *base;
    uint32_t count;
    uint32_t data_off;
    uint8_t w;
    uint8_t h;
    uint8_t stride;
    const char *name;
} cjk_blob_t;

extern const uint8_t cjk12_bin_start[] asm("_binary_cjk12_bin_start");
extern const uint8_t cjk16_bin_start[] asm("_binary_cjk16_bin_start");

static cjk_blob_t s_b12;
static cjk_blob_t s_b16;
static bool s_inited;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void blob_init(cjk_blob_t *b, const uint8_t *base, const char *name)
{
    memset(b, 0, sizeof(*b));
    if (base == NULL || memcmp(base, "S31F", 4) != 0) {
        return;
    }
    b->base = base;
    b->w = base[4];
    b->h = base[5];
    b->stride = (uint8_t)((uint16_t)base[6] | ((uint16_t)base[7] << 8));
    b->count = rd32(base + 8);
    b->data_off = 12 + b->count * 8;
    b->name = name;
}

static void ensure_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    blob_init(&s_b12, cjk12_bin_start, "cjk12");
    blob_init(&s_b16, cjk16_bin_start, "cjk16");
    ESP_LOGI("cjk", "font tables: %s=%u glyphs %ux%u, %s=%u glyphs %ux%u", s_b12.name,
             (unsigned)s_b12.count, s_b12.w, s_b12.h, s_b16.name, (unsigned)s_b16.count, s_b16.w, s_b16.h);
}

static const cjk_blob_t *pick(uint8_t px_size)
{
    ensure_init();
    if (px_size >= 16 && s_b16.count) {
        return &s_b16;
    }
    return s_b12.count ? &s_b12 : (s_b16.count ? &s_b16 : NULL);
}

static bool find(const cjk_blob_t *b, uint32_t cp, uint32_t *off)
{
    uint32_t lo = 0, hi = b->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = b->base + 12 + mid * 8;
        uint32_t entry_cp = rd32(e);
        if (entry_cp == cp) {
            *off = rd32(e + 4);
            return true;
        }
        if (entry_cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

const uint8_t *ui_cjk_glyph(uint32_t codepoint, uint8_t px_size, uint8_t *w, uint8_t *h, uint8_t *stride)
{
    const cjk_blob_t *b = pick(px_size);
    if (b == NULL) {
        return NULL;
    }
    uint32_t off;
    if (!find(b, codepoint, &off)) {
        return NULL;
    }
    if (w) {
        *w = b->w;
    }
    if (h) {
        *h = b->h;
    }
    if (stride) {
        *stride = b->stride;
    }
    return b->base + b->data_off + off;
}

bool ui_cjk_has(uint32_t codepoint, uint8_t px_size)
{
    const cjk_blob_t *b = pick(px_size);
    if (b == NULL) {
        return false;
    }
    uint32_t off;
    return find(b, codepoint, &off);
}

uint32_t ui_cjk_count(uint8_t px_size)
{
    const cjk_blob_t *b = pick(px_size);
    return b ? b->count : 0;
}
