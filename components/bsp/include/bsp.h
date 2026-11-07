/*
 * bsp.h —— 慧勤智远 ESP32-S31 CB V1.1 板级定义
 *
 * 引脚来源：官方《ESP32-S31 CB V1.1 用户手册 V1.0》表 1.1「引脚分配表」
 *   - TF 卡：模组 27~32 脚 SD_D0/D1/D2/D3/CLK/CMD，SDIO 4-bit 专用 IOMUX，
 *            在 IDF 里不需要（也不能）指定 GPIO 号。
 *   - I2C  ：IO0 = TP_SDA/CDATA/DVP_SDA，IO1 = TP_SCL/CCLK/DVP_SCL
 *            这是板上共用的一条 I2C 总线（触摸、摄像头、ES8389 音频 Codec）。
 *   - BOOT ：IO61； RGB 灯：IO37； 背光：IO38； 屏复位：IO60
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------ I2C ------------------------------ */
#define BSP_I2C_PORT      0
/* Board I2C bus shared by the LCD touch controller and audio codec. */
#define BSP_I2C_SDA_GPIO  0
#define BSP_I2C_SCL_GPIO  1
#define BSP_I2C_FREQ_HZ   400000

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_bus(void);

/* Add a device to the shared I2C bus. */
esp_err_t bsp_i2c_add_device(uint8_t addr, uint32_t speed_hz, i2c_master_dev_handle_t *out_dev);

/* 扫描总线，返回发现的地址列表（调试用） */
esp_err_t bsp_i2c_scan(uint8_t *addrs, size_t max, size_t *found);

/* ------------------------------ SD 卡 ------------------------------ */
#define BSP_SD_MOUNT_POINT "/sdcard"

esp_err_t bsp_sd_mount(void);
esp_err_t bsp_sd_unmount(void);
/* Blocks new application mounts while OTA exclusively owns the card. */
void bsp_sd_set_exclusive(bool exclusive);
bool bsp_sd_is_mounted(void);
/* Convert a path under /sdcard to the mounted card's FatFs drive path. */
bool bsp_sd_fat_path(const char *vfs_path, char *out, size_t out_size);

/* FAT 分区总容量、剩余容量（字节）；未挂载返回 ESP_ERR_INVALID_STATE */
esp_err_t bsp_sd_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* SD 卡 CSD 报告的原始容量（字节）。用于 FAT 统计不可用时的显示兜底。 */
uint64_t bsp_sd_card_capacity_bytes(void);

/* ------------------------------ RGB 灯 ------------------------------ */
esp_err_t bsp_led_init(void);
esp_err_t bsp_led_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_led_off(void);

#ifdef __cplusplus
}
#endif
