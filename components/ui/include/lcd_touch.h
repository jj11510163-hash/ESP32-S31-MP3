#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_touch_init(void);

typedef enum {
    LCD_TOUCH_NONE,
    LCD_TOUCH_POINT,
    LCD_TOUCH_RELEASE,
} lcd_touch_event_t;

/* A missing update is distinct from the controller's explicit release report. */
lcd_touch_event_t lcd_touch_poll(uint16_t *x, uint16_t *y);
