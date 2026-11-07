#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LCD_W 480
#define LCD_H 320

esp_err_t lcd_panel_init(void);
bool lcd_panel_present(void);
esp_err_t lcd_panel_flush(const uint16_t *rgb565);
esp_err_t lcd_panel_flush_rect(const uint16_t *rgb565, int x, int y, int w, int h);
void lcd_panel_backlight(bool on);
bool lcd_panel_brightness_available(void);
esp_err_t lcd_panel_set_brightness(uint8_t level); /* 0..4, dimmest to brightest */
