/*
 * GBWUI — Grind By Weight UI
 * Application entry point
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "TCA9554PWR.h"
#include "I2C_Driver.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "lvgl.h"
#include "screen_main.h"

/* ── Entry point ────────────────────────────────────────────── */
void app_main(void)
{
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    screen_main_load();

    while (1)
    {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
