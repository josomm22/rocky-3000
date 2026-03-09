#include "LVGL_Driver.h"

static const char *LVGL_TAG = "LVGL";

lv_display_t *disp = NULL;
lv_indev_t *indev = NULL;

static void *buf1 = NULL;
static void *buf2 = NULL;
/* Scratch buffer for software rotation (physical panel size) */
static void *rot_buf = NULL;

/* Counts flushes completed; read by FPS timer in main */
volatile uint32_t lvgl_flush_count = 0;

void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    const uint8_t *src = px_map;

    /* Dirty tile dimensions in logical coordinates */
    int32_t lw = area->x2 - area->x1 + 1;
    int32_t lh = area->y2 - area->y1 + 1;

    /* Physical panel target rectangle (default: logical == physical, no rotation) */
    int32_t px1 = area->x1, py1 = area->y1;
    int32_t px2 = area->x2 + 1, py2 = area->y2 + 1; /* exclusive end */

    if (rotation != LV_DISPLAY_ROTATION_0)
    {
        lv_color_format_t cf = lv_display_get_color_format(disp);
        /* In PARTIAL mode the stride covers only the tile width, not the full display */
        uint32_t src_stride = lv_draw_buf_width_to_stride(lw, cf);
        /* ROT_270 output: physical-width = lh, physical-height = lw */
        uint32_t dst_stride = lv_draw_buf_width_to_stride(lh, cf);
        lv_draw_sw_rotate(px_map, rot_buf, lw, lh, src_stride, dst_stride, rotation, cf);
        src = (const uint8_t *)rot_buf;

        /* Coordinate mapping for ROT_270 (90° CCW):
         *   logical (lx, ly)  →  physical (HRES-1-ly, lx)
         *   tile (x1..x2, y1..y2) →:
         *     phys_x ∈ [HRES-1-y2 .. HRES-1-y1]  (x_end = HRES-y1, exclusive)
         *     phys_y ∈ [x1 .. x2]                  (y_end = x2+1,    exclusive)
         */
        px1 = EXAMPLE_LCD_H_RES - 1 - area->y2;
        py1 = area->x1;
        px2 = EXAMPLE_LCD_H_RES - area->y1;
        py2 = area->x2 + 1;
    }

#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    xSemaphoreGive(sem_gui_ready);
    xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
#endif
    esp_lcd_panel_draw_bitmap(panel_handle, px1, py1, px2, py2, src);
    lvgl_flush_count++;
    lv_display_flush_ready(disp);
}

void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void example_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;

    esp_lcd_touch_handle_t tp_handle = (esp_lcd_touch_handle_t)lv_indev_get_user_data(indev);

    esp_lcd_touch_read_data(tp_handle);
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(tp_handle, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0)
    {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void LVGL_Init(void)
{
    ESP_LOGI(LVGL_TAG, "Initialize LVGL library");
    lv_init();

    /* Create display with PHYSICAL dimensions */
    ESP_LOGI(LVGL_TAG, "Create LVGL display (physical 480x640)");
    disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_display_set_flush_cb(disp, example_lvgl_flush_cb);
    lv_display_set_user_data(disp, panel_handle);

    /* Set rotation BEFORE set_buffers so set_buffers captures the correct
     * logical width (640) and stride (1280 bytes/row) for the landscape buffer. */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

    ESP_LOGI(LVGL_TAG, "Allocate LVGL tile buffers in internal SRAM");
/* Partial-mode tile: 40 logical rows × 640 logical cols × 2 bytes = 51200 bytes.
 * Internal SRAM is ~6× faster than PSRAM and avoids cache-miss penalties from
 * the non-sequential writes that rotation produces. */
#define LVGL_TILE_ROWS 40
    size_t tile_size = (size_t)EXAMPLE_LCD_V_RES * LVGL_TILE_ROWS * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(tile_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(buf1);
    /* rot_buf is the software-rotation scratch buffer.  It is the SOURCE of a
     * CPU memcpy into the RGB framebuffer (not DMA'd directly), so PSRAM is
     * fine and avoids competing with the WiFi stack for internal SRAM. */
    rot_buf = heap_caps_malloc(tile_size, MALLOC_CAP_SPIRAM);
    assert(rot_buf);
    /* buf2 not needed; single-buffer partial mode is sufficient */

    /* PARTIAL mode: LVGL renders only dirty rectangles, flush_cb is called per tile */
    lv_display_set_buffers(disp, buf1, NULL, tile_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(LVGL_TAG, "Register touch input device");
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, example_touchpad_read);
    lv_indev_set_user_data(indev, tp);

    ESP_LOGI(LVGL_TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));
}