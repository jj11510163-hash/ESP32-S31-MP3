#include "lcd_cover.h"
#include "cover_image_decoder.h"
#include "music_cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music.h"

#define COVER_W 218
#define COVER_H 212
#define MAX_COVER_BYTES (4 * 1024 * 1024)
#define COVER_READ_CHUNK (16 * 1024)
#define MAX_HARDWARE_EDGE 2048
#define THUMB_RETRY_US (60LL * 1000000)

static const char *TAG = "lcd_cover";
static uint16_t *s_cover;
static bool s_available;
static QueueHandle_t s_thumb_queue;
static SemaphoreHandle_t s_decode_lock;
static atomic_bool s_thumb_busy;
static atomic_bool s_thumb_updated;
static atomic_uint s_thumb_generation;
typedef struct {
    int slot;
    unsigned generation;
    char path[MUSIC_PATH_MAX];
} thumb_request_t;
static struct {
    char path[MUSIC_PATH_MAX];
    uint16_t *pixels;
    int64_t attempted_at_us;
    bool attempted;
    atomic_bool available;
} s_thumbs[LCD_COVER_THUMB_SLOTS];

static bool decode_jpeg_hardware(const uint8_t *encoded, size_t bytes,
                                 uint16_t *target, int target_w, int target_h)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(encoded, bytes, &info) != ESP_OK ||
        !info.width || !info.height ||
        info.width > MAX_HARDWARE_EDGE || info.height > MAX_HARDWARE_EDGE) return false;
    uint32_t mcu_w = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ||
                     info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    uint32_t mcu_h = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    uint32_t padded_w = (info.width + mcu_w - 1) & ~(mcu_w - 1);
    uint32_t padded_h = (info.height + mcu_h - 1) & ~(mcu_h - 1);
    size_t output_bytes = (size_t)padded_w * padded_h * 2;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t allocated = 0;
    uint8_t *decoded = jpeg_alloc_decoder_mem(output_bytes, &mem_cfg, &allocated);
    if (!decoded) return false;
    jpeg_decode_engine_cfg_t engine_cfg = {.timeout_ms = 400};
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    uint32_t decoded_bytes = 0;
    if (err == ESP_OK) {
        jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        };
        err = jpeg_decoder_process(decoder, &cfg, encoded, bytes,
                                   decoded, allocated, &decoded_bytes);
        jpeg_del_decoder_engine(decoder);
    }
    bool available = err == ESP_OK && decoded_bytes == output_bytes &&
                     output_bytes <= allocated;
    if (available) {
        /* RGB output is byte ordered; convert each pair to our RGB565 word. */
        uint32_t side = info.width < info.height ? info.width : info.height;
        uint32_t left = (info.width - side) / 2;
        uint32_t top = (info.height - side) / 2;
        for (int y = 0; y < target_h; y++) {
            uint32_t src_y = top + (uint32_t)y * side / target_h;
            for (int x = 0; x < target_w; x++) {
                uint32_t src_x = left + (uint32_t)x * side / target_w;
                uint32_t pixel = (src_y * padded_w + src_x) * 2;
                target[y * target_w + x] = ((uint16_t)decoded[pixel] << 8) | decoded[pixel + 1];
            }
        }
    }
    free(decoded);
    return available;
}

static bool decode_source(FILE *file, uint32_t embedded_bytes,
                          uint16_t *target, int target_w, int target_h)
{
    long bytes = embedded_bytes;
    if (!embedded_bytes) {
        if (fseek(file, 0, SEEK_END) != 0) return false;
        bytes = ftell(file);
        if (fseek(file, 0, SEEK_SET) != 0) return false;
    }
    if (bytes <= 0 || bytes > MAX_COVER_BYTES) return false;

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    size_t allocated = 0;
    uint8_t *encoded = jpeg_alloc_decoder_mem(bytes, &mem_cfg, &allocated);
    if (!encoded) return false;
    size_t read_bytes = 0;
    while (read_bytes < (size_t)bytes) {
        size_t chunk = (size_t)bytes - read_bytes;
        if (chunk > COVER_READ_CHUNK) chunk = COVER_READ_CHUNK;
        size_t got = fread(encoded + read_bytes, 1, chunk, file);
        if (got != chunk) break;
        read_bytes += got;
    }
    bool read_ok = read_bytes == (size_t)bytes;
    bool available = false;
    if (read_ok) {
        bool jpeg = bytes >= 3 && encoded[0] == 0xFF && encoded[1] == 0xD8 &&
                    encoded[2] == 0xFF;
        bool png = bytes >= 8 && memcmp(encoded, "\x89PNG\r\n\x1a\n", 8) == 0;
        if (jpeg) available = decode_jpeg_hardware(encoded, bytes, target, target_w, target_h);
        if (!available && (jpeg || png)) {
            available = cover_image_decode_software(encoded, bytes, target, target_w, target_h);
            static bool logged_stack;
            if (!logged_stack) {
                ESP_LOGI(TAG, "software cover decode: %s, stack low watermark=%u bytes",
                         available ? "ok" : "failed",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                logged_stack = true;
            }
        }
    }
    free(encoded);
    return available;
}

static bool try_path(const char *path, uint16_t *target, int target_w, int target_h)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool available = decode_source(file, 0, target, target_w, target_h);
    fclose(file);
    if (!available) ESP_LOGW(TAG, "cover source unavailable: %s", path);
    return available;
}

static bool decode_cover(const char *track_path, uint16_t *target,
                         int target_w, int target_h)
{
    if (!track_path || !track_path[0]) return false;
    char path[MUSIC_PATH_MAX + 32];
    size_t length = strlen(track_path);
    if (length >= sizeof(path)) return false;
    const char *slash = strrchr(track_path, '/');
    const char *dot = strrchr(track_path, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - track_path) : length;
    static const char *same_name[] = {".jpg", ".jpeg", ".png"};
    for (size_t i = 0; i < sizeof(same_name) / sizeof(same_name[0]); i++) {
        size_t ext_len = strlen(same_name[i]);
        if (stem + ext_len >= sizeof(path)) continue;
        memcpy(path, track_path, stem);
        memcpy(path + stem, same_name[i], ext_len + 1);
        if (try_path(path, target, target_w, target_h)) return true;
    }

    uint32_t offset, bytes;
    if (music_cover_find(track_path, &offset, &bytes)) {
        FILE *file = fopen(track_path, "rb");
        if (file) {
            bool available = fseek(file, (long)offset, SEEK_SET) == 0 &&
                             decode_source(file, bytes, target, target_w, target_h);
            fclose(file);
            if (available) return true;
            ESP_LOGW(TAG, "embedded cover unavailable: %s", track_path);
        }
    }

    if (!slash) return false;
    size_t directory = (size_t)(slash - track_path) + 1;
    static const char *folder_names[] = {
        "folder.jpg", "folder.jpeg", "folder.png",
        "cover.jpg", "cover.jpeg", "cover.png",
        "front.jpg", "front.jpeg", "front.png",
        "album.jpg", "album.jpeg", "album.png", "AlbumArtSmall.jpg",
    };
    for (size_t i = 0; i < sizeof(folder_names) / sizeof(folder_names[0]); i++) {
        size_t name_len = strlen(folder_names[i]);
        if (directory + name_len >= sizeof(path)) continue;
        memcpy(path, track_path, directory);
        memcpy(path + directory, folder_names[i], name_len + 1);
        if (try_path(path, target, target_w, target_h)) return true;
    }
    return false;
}

void lcd_cover_load(const char *track_path)
{
    s_available = false;
    if (!track_path || !track_path[0]) return;
    if (!s_cover) {
        s_cover = heap_caps_malloc(COVER_W * COVER_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_cover) {
        if (s_decode_lock) xSemaphoreTake(s_decode_lock, portMAX_DELAY);
        s_available = decode_cover(track_path, s_cover, COVER_W, COVER_H);
        if (s_decode_lock) xSemaphoreGive(s_decode_lock);
    }
}

bool lcd_cover_available(void) { return s_available; }
const uint16_t *lcd_cover_pixels(void) { return s_available ? s_cover : NULL; }

void lcd_cover_thumb_clear(void)
{
    atomic_fetch_add(&s_thumb_generation, 1);
    for (int i = 0; i < LCD_COVER_THUMB_SLOTS; i++) {
        s_thumbs[i].path[0] = '\0';
        s_thumbs[i].attempted = false;
        atomic_store(&s_thumbs[i].available, false);
    }
}

static void thumb_task(void *arg)
{
    (void)arg;
    thumb_request_t request;
    for (;;) {
        if (xQueueReceive(s_thumb_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        bool available = false;
        if (request.generation == atomic_load(&s_thumb_generation)) {
            if (s_decode_lock) xSemaphoreTake(s_decode_lock, portMAX_DELAY);
            if (request.generation == atomic_load(&s_thumb_generation)) {
                available = decode_cover(request.path, s_thumbs[request.slot].pixels,
                                         LCD_COVER_THUMB_SIZE, LCD_COVER_THUMB_SIZE);
            }
            if (s_decode_lock) xSemaphoreGive(s_decode_lock);
        }
        if (request.generation == atomic_load(&s_thumb_generation)) {
            atomic_store_explicit(&s_thumbs[request.slot].available,
                                  available, memory_order_release);
            atomic_store(&s_thumb_updated, true);
        }
        atomic_store(&s_thumb_busy, false);
    }
}

bool lcd_cover_thumb_init(void)
{
    if (s_thumb_queue) return true;
    s_decode_lock = xSemaphoreCreateMutex();
    s_thumb_queue = xQueueCreate(1, sizeof(thumb_request_t));
    if (!s_decode_lock || !s_thumb_queue) goto failed;
    if (xTaskCreate(thumb_task, "cover_thumb", 20 * 1024, NULL, 2, NULL) == pdPASS)
        return true;
failed:
    if (s_thumb_queue) vQueueDelete(s_thumb_queue);
    if (s_decode_lock) vSemaphoreDelete(s_decode_lock);
    s_thumb_queue = NULL;
    s_decode_lock = NULL;
    return false;
}

bool lcd_cover_thumb_prepare(int slot, const char *track_path)
{
    if (!s_thumb_queue || slot < 0 || slot >= LCD_COVER_THUMB_SLOTS ||
        !track_path || !track_path[0])
        return false;
    int64_t now = esp_timer_get_time();
    if (s_thumbs[slot].attempted && strcmp(s_thumbs[slot].path, track_path) == 0 &&
        (atomic_load_explicit(&s_thumbs[slot].available, memory_order_acquire) ||
         now - s_thumbs[slot].attempted_at_us < THUMB_RETRY_US))
        return false;
    if (atomic_load(&s_thumb_busy)) return false;
    if (!s_thumbs[slot].pixels) {
        s_thumbs[slot].pixels = heap_caps_malloc(LCD_COVER_THUMB_SIZE * LCD_COVER_THUMB_SIZE * 2,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_thumbs[slot].pixels) return false;
    }
    thumb_request_t request = {.slot = slot, .generation = atomic_load(&s_thumb_generation)};
    snprintf(request.path, sizeof(request.path), "%s", track_path);
    snprintf(s_thumbs[slot].path, sizeof(s_thumbs[slot].path), "%s", track_path);
    s_thumbs[slot].attempted_at_us = now;
    s_thumbs[slot].attempted = true;
    atomic_store_explicit(&s_thumbs[slot].available, false, memory_order_release);
    atomic_store(&s_thumb_busy, true);
    if (xQueueSend(s_thumb_queue, &request, 0) == pdTRUE) return true;
    atomic_store(&s_thumb_busy, false);
    s_thumbs[slot].attempted = false;
    return false;
}

bool lcd_cover_thumb_take_update(void)
{
    return atomic_exchange(&s_thumb_updated, false);
}

const uint16_t *lcd_cover_thumb_pixels(int slot, const char *track_path)
{
    if (slot < 0 || slot >= LCD_COVER_THUMB_SLOTS || !track_path ||
        !atomic_load_explicit(&s_thumbs[slot].available, memory_order_acquire) ||
        strcmp(s_thumbs[slot].path, track_path) != 0)
        return NULL;
    return s_thumbs[slot].pixels;
}
