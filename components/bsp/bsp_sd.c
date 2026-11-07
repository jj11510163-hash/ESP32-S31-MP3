#include "bsp.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp_sd";

static sdmmc_card_t *s_card;
static bool s_mounted;
static volatile bool s_exclusive;

void bsp_sd_set_exclusive(bool exclusive)
{
    __atomic_store_n(&s_exclusive, exclusive, __ATOMIC_RELEASE);
}

esp_err_t bsp_sd_mount(void)
{
    if (__atomic_load_n(&s_exclusive, __ATOMIC_ACQUIRE)) return ESP_ERR_INVALID_STATE;
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* Initialize at the standard 20 MHz SDMMC frequency. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    /* ESP32-S31 defaults map CLK/CMD/D0-D3 to GPIO24/25/20-23.  The board
     * schematic has no external pull-ups; use the same pull-up setting as
     * the vendor's SDMMC example. */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mounting TF card (SDIO 4-bit) ...");
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s (0x%X)  宽度=%d 频率=%d kHz", esp_err_to_name(err), err,
                 slot.width, host.max_freq_khz);
        /* Retry at the 400 kHz probing frequency after a mount failure. */
        ESP_LOGW(TAG, "retry at 400kHz ...");
        host.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "仍然失败: %s (0x%X)", esp_err_to_name(err), err);
            s_card = NULL;
            s_mounted = false;
            return err;
        }
        ESP_LOGW(TAG, "400kHz 下挂载成功 —— 说明走线/卡质量在高速下不稳定");
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        s_mounted = false;
    }
    return err;
}

bool bsp_sd_is_mounted(void)
{
    return s_mounted;
}

bool bsp_sd_fat_path(const char *vfs_path, char *out, size_t out_size)
{
    const size_t prefix_len = sizeof(BSP_SD_MOUNT_POINT) - 1;
    if (!s_mounted || !s_card || !vfs_path || !out || out_size == 0 ||
        strncmp(vfs_path, BSP_SD_MOUNT_POINT, prefix_len) != 0 ||
        (vfs_path[prefix_len] != '/' && vfs_path[prefix_len] != '\0')) {
        return false;
    }

    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xff) return false;

    int n = snprintf(out, out_size, "%u:%s", (unsigned)pdrv, vfs_path + prefix_len);
    return n >= 0 && (size_t)n < out_size;
}

uint64_t bsp_sd_card_capacity_bytes(void)
{
    if (!s_mounted || s_card == NULL || s_card->csd.capacity <= 0 || s_card->csd.sector_size <= 0) {
        return 0;
    }

    /* CSD capacity is expressed as a number of sectors. Cast before
     * multiplying so the result remains 64-bit on all supported targets. */
    return (uint64_t)(unsigned)s_card->csd.capacity * (uint64_t)(unsigned)s_card->csd.sector_size;
}

esp_err_t bsp_sd_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    /* Callers use zero as the "not available" value.  Clear both outputs
     * before checking the mount state so a failed refresh cannot leave an
     * old capacity on screen. */
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (free_bytes != NULL) {
        *free_bytes = 0;
    }
    if (!s_mounted || s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t queried_total = 0;
    uint64_t queried_free = 0;
    esp_err_t err = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &queried_total, &queried_free);

    /* Some FATFS/card combinations can mount successfully while f_getfree()
     * temporarily returns an error or a tiny invalid value. The card's CSD
     * still provides a reliable physical capacity for diagnostics. */
    uint64_t card_total = bsp_sd_card_capacity_bytes();
    if (queried_total == 0 || (card_total != 0 && queried_total < 1024 * 1024ULL)) {
        queried_total = card_total;
    }

    if (total_bytes != NULL) {
        *total_bytes = queried_total;
    }
    if (free_bytes != NULL) {
        *free_bytes = queried_free;
    }

    return err;
}
