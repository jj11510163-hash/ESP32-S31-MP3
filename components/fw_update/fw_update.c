#include "fw_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music.h"
#include "net_service.h"

#define COPY_BYTES 4096

static const char *TAG = "fw_update";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static fw_update_info_t s_info;

static void status_copy(fw_update_info_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_info;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set(const fw_update_info_t *info)
{
    taskENTER_CRITICAL(&s_lock);
    s_info = *info;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_stage(fw_update_state_t state, uint8_t percent, const char *message)
{
    taskENTER_CRITICAL(&s_lock);
    s_info.state = state;
    s_info.percent = percent;
    snprintf(s_info.message, sizeof(s_info.message), "%s", message);
    taskEXIT_CRITICAL(&s_lock);
}

static bool valid_string(const char *s, size_t len)
{
    return memchr(s, '\0', len) != NULL && s[0] != '\0';
}

static esp_err_t check_image(FILE *file, const esp_partition_t *target,
                             fw_update_info_t *info)
{
    if (fseek(file, 0, SEEK_END) != 0) {
        snprintf(info->message, sizeof(info->message), "读取固件大小失败");
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size < (long)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                      sizeof(esp_app_desc_t)) ||
        size > (long)target->size || fseek(file, 0, SEEK_SET) != 0) {
        snprintf(info->message, sizeof(info->message), "固件大小无效或超过备用分区");
        return ESP_ERR_INVALID_SIZE;
    }
    info->image_size = (uint32_t)size;

    esp_image_header_t image;
    esp_image_segment_header_t segment;
    esp_app_desc_t desc;
    if (fread(&image, 1, sizeof(image), file) != sizeof(image) ||
        fread(&segment, 1, sizeof(segment), file) != sizeof(segment) ||
        fread(&desc, 1, sizeof(desc), file) != sizeof(desc)) {
        snprintf(info->message, sizeof(info->message), "固件头读取失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (image.magic != ESP_IMAGE_HEADER_MAGIC ||
        image.chip_id != ESP_CHIP_ID_ESP32S31 ||
        image.segment_count == 0 || image.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
        segment.data_len < sizeof(desc) ||
        segment.data_len > info->image_size - sizeof(image) - sizeof(segment) ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !valid_string(desc.project_name, sizeof(desc.project_name)) ||
        !valid_string(desc.version, sizeof(desc.version))) {
        snprintf(info->message, sizeof(info->message), "不是有效的 S31 应用固件");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    snprintf(info->candidate_version, sizeof(info->candidate_version), "%s", desc.version);
    if (strcmp(desc.project_name, running->project_name) != 0) {
        snprintf(info->message, sizeof(info->message), "固件项目不匹配");
        return ESP_ERR_INVALID_ARG;
    }
    char *end = NULL;
    unsigned long major = strtoul(desc.version, &end, 10);
    if (end == desc.version || major < 2 || (*end != '.' && *end != '\0')) {
        snprintf(info->message, sizeof(info->message), "固件版本过旧，不能保留更新功能");
        return ESP_ERR_INVALID_VERSION;
    }
    if (strcmp(desc.version, running->version) == 0) {
        snprintf(info->message, sizeof(info->message), "当前已是此版本");
        return ESP_ERR_INVALID_VERSION;
    }

    info->available = true;
    snprintf(info->message, sizeof(info->message), "可以更新");
    return ESP_OK;
}

static esp_err_t open_image(FILE **file_out, const esp_partition_t **target_out,
                            fw_update_info_t *info)
{
    info->available = false;
    if (!bsp_sd_is_mounted()) {
        snprintf(info->message, sizeof(info->message), "请插入 SD 卡");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        snprintf(info->message, sizeof(info->message), "未找到备用固件分区");
        return ESP_ERR_INVALID_STATE;
    }
    FILE *file = fopen(FW_UPDATE_PATH, "rb");
    if (!file) {
        snprintf(info->message, sizeof(info->message), "未找到 /update/firmware.bin");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = check_image(file, target, info);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        snprintf(info->message, sizeof(info->message), "固件文件定位失败");
        return ESP_FAIL;
    }
    *file_out = file;
    *target_out = target;
    return ESP_OK;
}

esp_err_t fw_update_probe(fw_update_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    fw_update_info_t info;
    status_copy(&info);
    if (info.state == FW_UPDATE_PREPARING || info.state == FW_UPDATE_RUNNING ||
        info.state == FW_UPDATE_SUCCESS) {
        *out = info;
        return ESP_ERR_INVALID_STATE;
    }
    memset(&info, 0, sizeof(info));
    info.state = FW_UPDATE_IDLE;
    snprintf(info.current_version, sizeof(info.current_version), "%s",
             esp_app_get_description()->version);
    FILE *file = NULL;
    const esp_partition_t *target = NULL;
    esp_err_t err = open_image(&file, &target, &info);
    if (file) fclose(file);
    (void)target;
    taskENTER_CRITICAL(&s_lock);
    if (s_info.state == FW_UPDATE_PREPARING || s_info.state == FW_UPDATE_RUNNING ||
        s_info.state == FW_UPDATE_SUCCESS) {
        *out = s_info;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_info = info;
    *out = info;
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

void fw_update_get_status(fw_update_info_t *out)
{
    if (!out) return;
    status_copy(out);
    if (!out->current_version[0]) {
        snprintf(out->current_version, sizeof(out->current_version), "%s",
                 esp_app_get_description()->version);
    }
}

bool fw_update_busy(void)
{
    taskENTER_CRITICAL(&s_lock);
    fw_update_state_t state = s_info.state;
    taskEXIT_CRITICAL(&s_lock);
    return state == FW_UPDATE_PREPARING || state == FW_UPDATE_RUNNING ||
           state == FW_UPDATE_SUCCESS;
}

static void fail_update(esp_err_t err, const char *message)
{
    ESP_LOGE(TAG, "%s: %s", message, esp_err_to_name(err));
    music_sd_release(MUSIC_SD_OWNER_OTA);
    status_stage(FW_UPDATE_FAILED, 0, message);
}

static void update_task(void *arg)
{
    (void)arg;
    FILE *file = NULL;
    const esp_partition_t *target = NULL;
    esp_ota_handle_t handle = 0;
    bool handle_active = false;
    uint8_t *buffer = NULL;
    esp_err_t err = ESP_FAIL;
    const char *failure = "更新失败";
    fw_update_info_t info;
    status_copy(&info);

    if (music_lib_scanning()) {
        err = ESP_ERR_INVALID_STATE;
        failure = "曲库扫描中，请稍后重试";
        goto failed;
    }
    if (net_service_ap_running()) {
        err = ESP_ERR_INVALID_STATE;
        failure = "请先关闭无线传歌";
        goto failed;
    }
    if (net_service_sd_busy()) {
        err = ESP_ERR_INVALID_STATE;
        failure = "无线传歌仍在处理，请稍后重试";
        goto failed;
    }
    err = player_stop();
    if (err != ESP_OK) {
        failure = "停止播放失败";
        goto failed;
    }

    err = open_image(&file, &target, &info);
    status_set(&info);
    if (err != ESP_OK) {
        failure = info.message;
        goto failed;
    }
    buffer = heap_caps_malloc(COPY_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) {
        err = ESP_ERR_NO_MEM;
        failure = "内存不足";
        goto failed;
    }
    status_stage(FW_UPDATE_PREPARING, 0, "准备备用分区");
    err = esp_ota_begin(target, info.image_size, &handle);
    if (err != ESP_OK) {
        failure = "准备备用分区失败";
        goto failed;
    }
    handle_active = true;
    status_stage(FW_UPDATE_RUNNING, 0, "正在写入固件");
    uint32_t written = 0;
    while (written < info.image_size) {
        size_t want = info.image_size - written;
        if (want > COPY_BYTES) want = COPY_BYTES;
        size_t count = fread(buffer, 1, want, file);
        if (count != want) {
            err = ESP_FAIL;
            failure = "读取 SD 卡失败";
            goto failed;
        }
        err = esp_ota_write(handle, buffer, count);
        if (err != ESP_OK) {
            failure = "写入备用分区失败";
            goto failed;
        }
        written += (uint32_t)count;
        uint8_t percent = (uint8_t)((uint64_t)written * 100 / info.image_size);
        taskENTER_CRITICAL(&s_lock);
        s_info.percent = percent;
        taskEXIT_CRITICAL(&s_lock);
    }
    fclose(file);
    file = NULL;
    free(buffer);
    buffer = NULL;
    status_stage(FW_UPDATE_RUNNING, 100, "正在校验固件");
    err = esp_ota_end(handle);
    handle_active = false;
    if (err != ESP_OK) {
        failure = "固件校验失败";
        goto failed;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        failure = "切换启动分区失败";
        goto failed;
    }
    ESP_LOGI(TAG, "update complete: %s (%lu bytes)", info.candidate_version,
             (unsigned long)info.image_size);
    status_stage(FW_UPDATE_SUCCESS, 100, "更新完成，即将重启");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

failed:
    if (handle_active) esp_ota_abort(handle);
    if (buffer) free(buffer);
    if (file) fclose(file);
    fail_update(err, failure);
    vTaskDelete(NULL);
}

esp_err_t fw_update_start(void)
{
    if (!music_sd_try_acquire(MUSIC_SD_OWNER_OTA)) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_lock);
    if (s_info.state == FW_UPDATE_PREPARING || s_info.state == FW_UPDATE_RUNNING ||
        s_info.state == FW_UPDATE_SUCCESS) {
        taskEXIT_CRITICAL(&s_lock);
        music_sd_release(MUSIC_SD_OWNER_OTA);
        return ESP_ERR_INVALID_STATE;
    }
    s_info.state = FW_UPDATE_PREPARING;
    s_info.percent = 0;
    if (!s_info.current_version[0]) {
        snprintf(s_info.current_version, sizeof(s_info.current_version), "%s",
                 esp_app_get_description()->version);
    }
    snprintf(s_info.message, sizeof(s_info.message), "正在检查更新包");
    taskEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(update_task, "fw_update", 6144, NULL, 4, NULL) != pdPASS) {
        music_sd_release(MUSIC_SD_OWNER_OTA);
        status_stage(FW_UPDATE_FAILED, 0, "无法启动更新任务");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
