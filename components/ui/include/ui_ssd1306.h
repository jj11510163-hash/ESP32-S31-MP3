/*
 * ui_ssd1306.h —— SSD1306 128x64 I2C 面板驱动（1bpp）
 * 面板使用板上 GPIO 矩阵 I2C（SDA=IO8, SCL=IO9），默认地址 0x3C。
 * IO8/IO9 同时标为 RGB 屏 LCD_B3/LCD_B4；本项目不启用 RGB 屏时作为 OLED I2C。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_I2C_ADDR_PRIMARY 0x3C
#define SSD1306_I2C_ADDR_SECONDARY 0x3D

/* 初始化面板（会尝试 0x3C / 0x3D） */
esp_err_t ssd1306_init(void);
bool ssd1306_present(void);
uint8_t ssd1306_addr(void);

esp_err_t ssd1306_set_contrast(uint8_t contrast); /* 0..255 */
esp_err_t ssd1306_display_on(bool on);
esp_err_t ssd1306_invert(bool on);

/*
 * 把行优先 1bpp framebuffer 刷到面板。
 * page_mask 的 bit0..bit7 对应 8 个 page（每 page 8 行），只刷新置位的页，
 * 这样局部动画只需要发送变化的 128 字节。
 */
esp_err_t ssd1306_flush(const uint8_t *fb, uint8_t page_mask);

/* 列偏移：SSD1306=0，SH1106（1.3 寸常见）需要 2，否则最右边 4 列花屏 */
void ssd1306_set_col_offset(uint8_t off);
uint8_t ssd1306_col_offset(void);

#ifdef __cplusplus
}
#endif
