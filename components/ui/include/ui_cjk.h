/*
 * ui_cjk.h —— 中日文字库（从 assets/cjk12.bin、cjk16.bin 读，编译期嵌入 flash）
 *
 * 覆盖：GB2312 全字库(6763 汉字+符号) / JIS 第一水准汉字 / 平假名·片假名 /
 *       CJK 标点 / 全角字符 / Latin-1 补充，共 9386 个码位。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回字形位图（1bpp，MSB 在左），找不到返回 NULL */
const uint8_t *ui_cjk_glyph(uint32_t codepoint, uint8_t px_size, uint8_t *w, uint8_t *h, uint8_t *stride);

bool ui_cjk_has(uint32_t codepoint, uint8_t px_size);

/* 字库统计（UI 关于页可用） */
uint32_t ui_cjk_count(uint8_t px_size);

#ifdef __cplusplus
}
#endif
