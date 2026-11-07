/*
 * ui.h —— LCD 播放页和曲库页对外接口
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 ST7796U 面板、GT1151Q 触摸和 LCD 页面任务 */
esp_err_t ui_init(void);

/* 面板是否存在（无屏时其余功能照常跑） */
bool ui_has_panel(void);

#ifdef __cplusplus
}
#endif
