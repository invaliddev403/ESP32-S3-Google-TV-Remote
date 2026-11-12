#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 480

esp_err_t board_display_init(esp_lcd_panel_handle_t *panel);
esp_err_t board_touch_init(void);
bool board_touch_read(int32_t *x, int32_t *y);
void board_backlight_set(bool on);
void board_backlight_set_brightness(uint8_t percent);
