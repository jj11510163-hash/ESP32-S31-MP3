#include "cover_image_decoder.h"

#include <limits.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC_SIZED(ptr, old_size, new_size) \
    heap_caps_realloc((ptr), (new_size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(ptr) free(ptr)
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define MAX_ART_PIXELS 2500000U

static const char *TAG = "cover_decode";

bool cover_image_decode_software(const uint8_t *encoded, size_t bytes,
                                 uint16_t *target, int target_w, int target_h)
{
    if (!encoded || !target || bytes == 0 || bytes > INT_MAX ||
        target_w <= 0 || target_h <= 0 || target_w > 512 || target_h > 512) {
        return false;
    }

    int width = 0;
    int height = 0;
    int components = 0;
    if (!stbi_info_from_memory(encoded, (int)bytes, &width, &height, &components)) {
        ESP_LOGW(TAG, "artwork header unsupported: %s", stbi_failure_reason());
        return false;
    }
    if (width < 32 || height < 32 || width > STBI_MAX_DIMENSIONS ||
        height > STBI_MAX_DIMENSIONS ||
        (uint64_t)width * height > MAX_ART_PIXELS) {
        ESP_LOGW(TAG, "artwork size unsupported: %dx%d", width, height);
        return false;
    }

    int channels = (components == 2 || components == 4) ? 4 : 3;
    stbi_uc *pixels = stbi_load_from_memory(encoded, (int)bytes, &width, &height,
                                            &components, channels);
    if (!pixels) {
        ESP_LOGW(TAG, "software artwork decode failed: %s", stbi_failure_reason());
        return false;
    }

    int side = width < height ? width : height;
    int left = (width - side) / 2;
    int top = (height - side) / 2;
    for (int y = 0; y < target_h; y++) {
        int src_y = top + (int)((int64_t)y * side / target_h);
        for (int x = 0; x < target_w; x++) {
            int src_x = left + (int)((int64_t)x * side / target_w);
            const stbi_uc *pixel = pixels + ((size_t)src_y * width + src_x) * channels;
            unsigned r = pixel[0];
            unsigned g = pixel[1];
            unsigned b = pixel[2];
            if (channels == 4) {
                unsigned alpha = pixel[3];
                r = (r * alpha + 255U * (255U - alpha) + 127U) / 255U;
                g = (g * alpha + 255U * (255U - alpha) + 127U) / 255U;
                b = (b * alpha + 255U * (255U - alpha) + 127U) / 255U;
            }
            target[(size_t)y * target_w + x] =
                (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
        }
    }

    stbi_image_free(pixels);
    return true;
}
