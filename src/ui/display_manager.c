/*
 * display_manager — screen brightness and sleep timeout
 *
 * Brightness and timeout are persisted in NVS namespace "disp_cfg".
 * Keys: "brightness" (u8, 10–100, default 80)
 *       "timeout_min" (u8, 0–60, default 10; 0 = never)
 *
 * Sleep is detected via LVGL's lv_display_get_inactive_time() polled every
 * second. On timeout the backlight is cut to 0. The first touch after sleep
 * wakes the display (restores brightness) without passing the event to the UI.
 */

#include "display_manager.h"
#include "LVGL_Driver.h" /* extern lv_display_t *disp */
#include "ST7701S.h"     /* Set_Backlight() */
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "disp_mgr";

#define NVS_NS "disp_cfg"
#define DEFAULT_BRIGHTNESS 80
#define DEFAULT_TIMEOUT_MIN 10

static uint8_t s_brightness = DEFAULT_BRIGHTNESS;
static uint8_t s_timeout_min = DEFAULT_TIMEOUT_MIN;
static bool s_sleeping = false;

/* ── NVS ──────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t v;
    if (nvs_get_u8(h, "brightness", &v) == ESP_OK && v >= 10)
        s_brightness = v;
    if (nvs_get_u8(h, "timeout_min", &v) == ESP_OK)
        s_timeout_min = v;
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "brightness", s_brightness);
    nvs_set_u8(h, "timeout_min", s_timeout_min);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Poll timer (1 s) ─────────────────────────────────────────── */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_sleeping || s_timeout_min == 0)
        return;

    uint32_t inactive_ms = lv_display_get_inactive_time(disp);
    uint32_t timeout_ms = (uint32_t)s_timeout_min * 60u * 1000u;
    if (inactive_ms >= timeout_ms)
    {
        s_sleeping = true;
        Set_Backlight(0);
        ESP_LOGI(TAG, "Screen sleeping after %u min idle", s_timeout_min);
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void disp_mgr_init(void)
{
    nvs_load();
    Set_Backlight(s_brightness);
    lv_timer_create(poll_cb, 1000, NULL);
    ESP_LOGI(TAG, "init: brightness=%u timeout=%u min", s_brightness, s_timeout_min);
}

bool disp_mgr_intercept_touch(void)
{
    if (!s_sleeping)
        return false;
    s_sleeping = false;
    Set_Backlight(s_brightness);
    ESP_LOGI(TAG, "Screen woken by touch");
    return true; /* caller must report LV_INDEV_STATE_RELEASED */
}

uint8_t disp_mgr_get_brightness(void) { return s_brightness; }

void disp_mgr_set_brightness(uint8_t pct)
{
    if (pct < 10)
        pct = 10;
    if (pct > 100)
        pct = 100;
    s_brightness = pct;
    if (!s_sleeping)
        Set_Backlight(pct);
    nvs_save();
}

uint8_t disp_mgr_get_timeout_min(void) { return s_timeout_min; }

void disp_mgr_set_timeout_min(uint8_t min)
{
    s_timeout_min = min;
    nvs_save();
}

bool disp_mgr_is_sleeping(void) { return s_sleeping; }
