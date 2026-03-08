#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "draw/sw/lv_draw_sw.h"

#include "ST7701S.h"
#include "GT911.h"

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2

extern lv_display_t *disp;
extern lv_indev_t *indev;

void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
void example_increase_lvgl_tick(void *arg);
void example_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data);

void LVGL_Init(void);

extern volatile uint32_t lvgl_flush_count;