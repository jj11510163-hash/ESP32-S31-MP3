#include "music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FAVORITES_MAX 400
#define FAVORITES_VERSION 1
#define FAVORITES_SLOT_A BSP_SD_MOUNT_POINT "/.s31_favorites_a"
#define FAVORITES_SLOT_B BSP_SD_MOUNT_POINT "/.s31_favorites_b"
#define FAVORITES_TEMP BSP_SD_MOUNT_POINT "/.s31_favorites_tmp"

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t generation;
    uint32_t count;
    uint32_t checksum;
} favorites_header_t;

static const char *const s_slots[2] = {FAVORITES_SLOT_A, FAVORITES_SLOT_B};
static const char *TAG = "favorites";
static const char s_magic[8] = {'S', '3', '1', 'F', 'A', 'V', '1', 0};
static SemaphoreHandle_t s_lock;
static char (*s_paths)[MUSIC_PATH_MAX];
static int s_path_count;
static int s_indices[FAVORITES_MAX];
static int s_index_count;
static int s_active_slot = -1;
static uint32_t s_generation;
static bool s_loaded;

static uint32_t checksum_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static bool valid_path(const char *path)
{
    static const char prefix[] = BSP_SD_MOUNT_POINT "/";
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    size_t length = strnlen(path, MUSIC_PATH_MAX);
    return length > sizeof(prefix) - 1 && length < MUSIC_PATH_MAX;
}

static int find_path(const char *path)
{
    for (int i = 0; i < s_path_count; ++i) {
        if (strcmp(s_paths[i], path) == 0) return i;
    }
    return -1;
}

static esp_err_t read_slot(const char *filename, favorites_header_t *out,
                           char (*paths)[MUSIC_PATH_MAX])
{
    FILE *file = fopen(filename, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;

    favorites_header_t header;
    esp_err_t result = ESP_FAIL;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        memcmp(header.magic, s_magic, sizeof(s_magic)) != 0 ||
        header.version != FAVORITES_VERSION || header.count > FAVORITES_MAX) goto done;

    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < header.count; ++i) {
        uint16_t length;
        char path[MUSIC_PATH_MAX];
        if (fread(&length, sizeof(length), 1, file) != 1 ||
            length == 0 || length >= sizeof(path) ||
            fread(path, 1, length, file) != length) goto done;
        path[length] = '\0';
        if (strlen(path) != length || !valid_path(path)) goto done;
        hash = checksum_bytes(hash, &length, sizeof(length));
        hash = checksum_bytes(hash, path, length);
        if (paths) memcpy(paths[i], path, length + 1);
    }
    if (hash != header.checksum || fgetc(file) != EOF || ferror(file)) goto done;
    *out = header;
    result = ESP_OK;

done:
    fclose(file);
    return result;
}

static esp_err_t ensure_loaded(void)
{
    if (!bsp_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (s_loaded) return ESP_OK;

    favorites_header_t headers[2];
    esp_err_t status[2];
    for (int i = 0; i < 2; ++i)
        status[i] = read_slot(s_slots[i], &headers[i], NULL);

    int chosen = -1;
    if (status[0] == ESP_OK) chosen = 0;
    if (status[1] == ESP_OK &&
        (chosen < 0 || (int32_t)(headers[1].generation - headers[0].generation) > 0))
        chosen = 1;
    if (chosen < 0) {
        if (status[0] != ESP_ERR_NOT_FOUND || status[1] != ESP_ERR_NOT_FOUND)
            return ESP_FAIL;
        s_path_count = 0;
        s_active_slot = -1;
        s_generation = 0;
    } else {
        favorites_header_t loaded;
        esp_err_t err = read_slot(s_slots[chosen], &loaded, s_paths);
        if (err != ESP_OK) return err;
        s_path_count = (int)loaded.count;
        s_active_slot = chosen;
        s_generation = loaded.generation;
    }
    s_loaded = true;
    ESP_LOGI(TAG, "loaded %d favorite paths", s_path_count);
    return ESP_OK;
}

static void rebuild_index(void)
{
    s_index_count = 0;
    for (int i = 0, count = music_lib_count(); i < count; ++i) {
        const music_item_t *item = music_lib_get(i);
        if (item && find_path(item->path) >= 0)
            s_indices[s_index_count++] = i;
    }
}

static esp_err_t write_slot(void)
{
    if (!bsp_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    favorites_header_t header = {
        .version = FAVORITES_VERSION,
        .generation = s_generation + 1,
        .count = (uint32_t)s_path_count,
        .checksum = 2166136261U,
    };
    memcpy(header.magic, s_magic, sizeof(s_magic));
    for (int i = 0; i < s_path_count; ++i) {
        uint16_t length = (uint16_t)strlen(s_paths[i]);
        header.checksum = checksum_bytes(header.checksum, &length, sizeof(length));
        header.checksum = checksum_bytes(header.checksum, s_paths[i], length);
    }

    FILE *file = fopen(FAVORITES_TEMP, "wb");
    if (!file) return ESP_FAIL;
    bool good = fwrite(&header, sizeof(header), 1, file) == 1;
    for (int i = 0; good && i < s_path_count; ++i) {
        uint16_t length = (uint16_t)strlen(s_paths[i]);
        good = fwrite(&length, sizeof(length), 1, file) == 1 &&
               fwrite(s_paths[i], 1, length, file) == length;
    }
    if (good) good = fflush(file) == 0;
    if (fclose(file) != 0) good = false;
    if (!good) {
        remove(FAVORITES_TEMP);
        return ESP_FAIL;
    }

    favorites_header_t checked;
    if (read_slot(FAVORITES_TEMP, &checked, NULL) != ESP_OK) {
        remove(FAVORITES_TEMP);
        return ESP_FAIL;
    }
    int next_slot = s_active_slot == 0 ? 1 : 0;
    if (remove(s_slots[next_slot]) != 0) {
        FILE *existing = fopen(s_slots[next_slot], "rb");
        if (existing) {
            fclose(existing);
            remove(FAVORITES_TEMP);
            return ESP_FAIL;
        }
    }
    if (rename(FAVORITES_TEMP, s_slots[next_slot]) != 0) {
        remove(FAVORITES_TEMP);
        return ESP_FAIL;
    }
    s_active_slot = next_slot;
    s_generation = header.generation;
    return ESP_OK;
}

esp_err_t music_favorites_init(void)
{
    if (s_lock) return ESP_OK;
    s_paths = heap_caps_malloc(FAVORITES_MAX * MUSIC_PATH_MAX,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_paths) s_paths = calloc(FAVORITES_MAX, MUSIC_PATH_MAX);
    if (!s_paths) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        free(s_paths);
        s_paths = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool music_favorites_contains(const char *path)
{
    if (!s_lock || !valid_path(path) || !bsp_sd_is_mounted() ||
        music_lib_scanning() || music_update_mode())
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = !music_lib_scanning() && !music_update_mode() &&
                 ensure_loaded() == ESP_OK &&
                 find_path(path) >= 0;
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t music_favorites_toggle(const char *path, bool *now_favorite)
{
    if (now_favorite) *now_favorite = false;
    if (!valid_path(path)) return ESP_ERR_INVALID_ARG;
    if (!s_lock || !bsp_sd_is_mounted() || music_update_mode())
        return ESP_ERR_INVALID_STATE;
    if (music_lib_scanning()) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = music_update_mode() ? ESP_ERR_INVALID_STATE : ensure_loaded();
    if (err == ESP_OK) {
        int found = find_path(path);
        if (found < 0 && s_path_count >= FAVORITES_MAX) {
            err = ESP_ERR_NO_MEM;
        } else {
            char removed[MUSIC_PATH_MAX];
            if (found >= 0) {
                memcpy(removed, s_paths[found], sizeof(removed));
                for (int i = found; i + 1 < s_path_count; ++i)
                    memcpy(s_paths[i], s_paths[i + 1], MUSIC_PATH_MAX);
                s_path_count--;
            } else {
                snprintf(s_paths[s_path_count++], MUSIC_PATH_MAX, "%s", path);
            }
            err = write_slot();
            if (err != ESP_OK) {
                if (found >= 0) {
                    for (int i = s_path_count; i > found; --i)
                        memcpy(s_paths[i], s_paths[i - 1], MUSIC_PATH_MAX);
                    memcpy(s_paths[found], removed, sizeof(removed));
                    s_path_count++;
                } else {
                    s_path_count--;
                }
            } else {
                rebuild_index();
                if (now_favorite) *now_favorite = found < 0;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

int music_favorites_count(void)
{
    if (!s_lock || !bsp_sd_is_mounted() || music_lib_scanning() || music_update_mode()) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = !music_lib_scanning() && !music_update_mode() && bsp_sd_is_mounted() ? s_index_count : 0;
    xSemaphoreGive(s_lock);
    return count;
}

int music_favorites_index_at(int position)
{
    if (!s_lock || !bsp_sd_is_mounted() || music_lib_scanning() || music_update_mode()) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = !music_lib_scanning() && !music_update_mode() && bsp_sd_is_mounted() &&
                position >= 0 && position < s_index_count ? s_indices[position] : -1;
    xSemaphoreGive(s_lock);
    return index;
}

esp_err_t music_favorites_refresh(void)
{
    if (!s_lock || music_update_mode()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_loaded = false;
    esp_err_t err = music_update_mode() ? ESP_ERR_INVALID_STATE : ensure_loaded();
    if (err == ESP_OK) rebuild_index();
    else s_index_count = 0;
    xSemaphoreGive(s_lock);
    return err;
}

void music_favorites_wait_idle(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreGive(s_lock);
}
