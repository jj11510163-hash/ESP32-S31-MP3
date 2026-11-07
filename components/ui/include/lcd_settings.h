#pragma once

#include <stdint.h>

#include "app_core.h"

typedef enum {
    LCD_SETTINGS_STAY = 0,
    LCD_SETTINGS_BACK,
    LCD_SETTINGS_GO_BT,
    LCD_SETTINGS_GO_HOME,
} lcd_settings_result_t;

void lcd_settings_enter(void);
bool lcd_settings_tick(void);
void lcd_settings_draw(uint16_t *frame);
bool lcd_settings_draw_volume(uint16_t *frame, int *top);
lcd_settings_result_t lcd_settings_touch(uint16_t x, uint16_t y);
bool lcd_settings_touch_begin(uint16_t x, uint16_t y);
bool lcd_settings_touch_move(uint16_t x, uint16_t y);
void lcd_settings_touch_end(void);
void lcd_settings_scroll(int rows);
lcd_settings_result_t lcd_settings_key(app_key_t key);
