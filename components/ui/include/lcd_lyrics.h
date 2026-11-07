#pragma once

#include <stdbool.h>
#include <stdint.h>

void lcd_lyrics_load(const char *track_path);
bool lcd_lyrics_available(void);
void lcd_lyrics_at(uint32_t pos_ms, const char **previous,
                   const char **current, const char **translation,
                   const char **next);
