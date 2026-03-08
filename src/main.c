#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "TCA9554PWR.h"
#include "I2C_Driver.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "lvgl.h"

static lv_obj_t *status_label;
static lv_obj_t *slider_label;
static lv_obj_t *fps_label;

/* --- FPS timer --- */
static void fps_timer_cb(lv_timer_t *t)
{
    static uint32_t last_count = 0;
    uint32_t cur = lvgl_flush_count;
    uint32_t fps = cur - last_count; /* called every 1000ms */
    last_count = cur;
    lv_label_set_text_fmt(fps_label, "%" LV_PRIu32 " FPS", fps);
}

/* --- Button callbacks --- */
static void btn1_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        lv_label_set_text(status_label, "Button 1 pressed");
}

static void btn2_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        lv_label_set_text(status_label, "Button 2 pressed");
}

/* --- Radio slider callback --- */
static void slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
    {
        lv_obj_t *slider = lv_event_get_target(e);
        int32_t val = lv_slider_get_value(slider);
        lv_label_set_text_fmt(slider_label, "Step: %d / 5", (int)val);
    }
}

/* Helper: create a styled button with a label */
static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb, int x_ofs, int y_ofs)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 180, 70);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
    return btn;
}

void app_main(void)
{
    /* Initialize hardware */
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    lv_obj_t *scr = lv_scr_act();

    /* Dark background */
    lv_obj_set_style_bg_color(scr, lv_color_make(20, 20, 30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* --- Title --- */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL Demo");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_make(100, 200, 255), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    /* --- Status label --- */
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Press a button below");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(status_label, lv_color_make(200, 200, 200), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -60);

    /* --- Buttons --- */
    create_button(scr, "Button 1", btn1_event_cb, -110, 30);
    create_button(scr, "Button 2", btn2_event_cb, 110, 30);

    /* --- Slider step label --- */
    slider_label = lv_label_create(scr);
    lv_label_set_text(slider_label, "Step: 1 / 5");
    lv_obj_set_style_text_font(slider_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(slider_label, lv_color_make(200, 200, 200), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(slider_label, LV_ALIGN_BOTTOM_MID, 0, -110);

    /* --- Discrete (radio-style) slider: 5 steps --- */
    lv_obj_t *slider = lv_slider_create(scr);
    lv_slider_set_range(slider, 1, 5);
    lv_slider_set_value(slider, 1, LV_ANIM_OFF);
    lv_obj_set_size(slider, 380, 12);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Step tick marks below the slider */
    static const char *step_labels[] = {"1", "2", "3", "4", "5"};
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *tick = lv_label_create(scr);
        lv_label_set_text(tick, step_labels[i]);
        lv_obj_set_style_text_font(tick, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(tick, lv_color_make(150, 150, 150), LV_PART_MAIN | LV_STATE_DEFAULT);
        /* Spread ticks evenly: slider is 380 px wide centred on screen (640 px) */
        int x = -190 + i * 95;
        lv_obj_align(tick, LV_ALIGN_BOTTOM_MID, x, -32);
    }

    /* --- FPS counter label (bottom-right) --- */
    fps_label = lv_label_create(scr);
    lv_label_set_text(fps_label, "-- FPS");
    lv_obj_set_style_text_font(fps_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(fps_label, lv_color_make(100, 100, 100), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(fps_label, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_timer_create(fps_timer_cb, 1000, NULL);

    /* Main LVGL loop */
    while (1)
    {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
