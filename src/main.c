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

static lv_obj_t *label;
static bool show_hello = true;

/* LVGL timer callback - runs in the LVGL task context, fully thread-safe */
static void toggle_text_cb(lv_timer_t *timer)
{
    show_hello = !show_hello;
    if (show_hello)
    {
        lv_label_set_text(label, "Hello World!");
    }
    else
    {
        lv_label_set_text(label, "How are you?");
    }
}

void app_main(void)
{
    /* Initialize hardware */
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    /* Black background */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Create centered label on the active screen */
    label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_make(220, 220, 220), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    /* Toggle text every 5 seconds via LVGL timer (runs in main loop, thread-safe) */
    lv_timer_create(toggle_text_cb, 5000, NULL);

    /* Main LVGL loop */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}
