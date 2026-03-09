/*
 * GBWUI — Settings screen (placeholder)
 *
 * Full design per DESIGN.md §5.4:
 *   WiFi [>], Pre-stop offset stepper, Brightness slider,
 *   Calibration [>], Firmware Update [>], Reset to defaults
 */

#include "lvgl.h"
#include <stdio.h>
#include "ui_palette.h"
#include "screen_settings.h"
#include "screen_main.h"
#include "screen_calibration.h"
#include "screen_wifi.h"
#include "screen_ota.h"
#include "display_manager.h"

/* ── Callbacks ──────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_main_load();
}

static void wifi_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_wifi_load();
}

static void calibration_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_calibration_load();
}

static void ota_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_ota_load();
}

/* ── Brightness slider callback ─────────────────────────────── */

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    disp_mgr_set_brightness((uint8_t)lv_slider_get_value(slider));
}

/* ── Timeout stepper state & callbacks ─────────────────────── */

static const uint8_t k_timeout_opts[] = {0, 1, 2, 5, 10, 15, 30, 60};
#define N_TIMEOUT_OPTS ((int)(sizeof(k_timeout_opts) / sizeof(k_timeout_opts[0])))

typedef struct
{
    int idx;
    lv_obj_t *lbl;
} timeout_ctx_t;
static timeout_ctx_t s_tctx; /* reset on each screen_settings_load() */

static void update_timeout_label(void)
{
    if (!s_tctx.lbl)
        return;
    uint8_t m = k_timeout_opts[s_tctx.idx];
    char buf[12];
    if (m == 0)
        lv_label_set_text(s_tctx.lbl, "Never");
    else
    {
        snprintf(buf, sizeof(buf), "%d min", m);
        lv_label_set_text(s_tctx.lbl, buf);
    }
}

static void timeout_dec_cb(lv_event_t *e)
{
    (void)e;
    if (s_tctx.idx > 0)
        s_tctx.idx--;
    disp_mgr_set_timeout_min(k_timeout_opts[s_tctx.idx]);
    update_timeout_label();
}

static void timeout_inc_cb(lv_event_t *e)
{
    (void)e;
    if (s_tctx.idx < N_TIMEOUT_OPTS - 1)
        s_tctx.idx++;
    disp_mgr_set_timeout_min(k_timeout_opts[s_tctx.idx]);
    update_timeout_label();
}

/* ── Helper: nav row with label + chevron ───────────────────── */
static void make_nav_row(lv_obj_t *parent, const char *label,
                         lv_event_cb_t cb, int y_ofs)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 580, 62);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y_ofs);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(btn, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(btn, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chevron = lv_label_create(btn);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(chevron, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* ── Public entry point ─────────────────────────────────────── */

void screen_settings_load(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Back button ────────────────────────────────────────── */
    lv_obj_t *btn_back = lv_button_create(scr);
    lv_obj_set_size(btn_back, 55, 55);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_radius(btn_back, 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_back, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_back, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_back, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_back);

    /* ── Title ──────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    /* ── Divider ────────────────────────────────────────────── */
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, SCR_W - 20, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(div, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Navigation rows ────────────────────────────────────── */
    make_nav_row(scr, "WiFi", wifi_cb, 90);
    make_nav_row(scr, "Calibration", calibration_cb, 166);
    make_nav_row(scr, "Firmware Update", ota_cb, 242);

    /* ── Brightness row ─────────────────────────────────────── */
    lv_obj_t *row_bright = lv_obj_create(scr);
    lv_obj_set_size(row_bright, 580, 62);
    lv_obj_align(row_bright, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_set_style_bg_color(row_bright, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row_bright, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(row_bright, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row_bright, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(row_bright, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(row_bright, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(row_bright, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_bright = lv_label_create(row_bright);
    lv_label_set_text(lbl_bright, "Brightness");
    lv_obj_set_style_text_font(lbl_bright, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_bright, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_bright, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(row_bright);
    lv_obj_set_size(slider, 220, 10);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, (int32_t)disp_mgr_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, COL_ACCENT, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Timeout row ────────────────────────────────────────── */
    /* Find the current timeout in the options table */
    s_tctx.idx = 4; /* default = 10 min */
    s_tctx.lbl = NULL;
    uint8_t cur_t = disp_mgr_get_timeout_min();
    for (int i = 0; i < N_TIMEOUT_OPTS; i++)
    {
        if (k_timeout_opts[i] == cur_t)
        {
            s_tctx.idx = i;
            break;
        }
    }

    lv_obj_t *row_to = lv_obj_create(scr);
    lv_obj_set_size(row_to, 580, 62);
    lv_obj_align(row_to, LV_ALIGN_TOP_MID, 0, 394);
    lv_obj_set_style_bg_color(row_to, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row_to, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(row_to, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row_to, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(row_to, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(row_to, 21, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(row_to, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_to_title = lv_label_create(row_to);
    lv_label_set_text(lbl_to_title, LV_SYMBOL_POWER "  Sleep after");
    lv_obj_set_style_text_font(lbl_to_title, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_to_title, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_to_title, LV_ALIGN_LEFT_MID, 0, 0);

    /* [-] value [+] group at right */
    lv_obj_t *btn_dec = lv_button_create(row_to);
    lv_obj_set_size(btn_dec, 36, 36);
    lv_obj_align(btn_dec, LV_ALIGN_RIGHT_MID, -120, 0);
    lv_obj_set_style_radius(btn_dec, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_dec, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_dec, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_dec, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_dec, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_dec, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn_dec, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_dec, timeout_dec_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl_dec, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_dec, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_dec);

    lv_obj_t *lbl_val = lv_label_create(row_to);
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_val, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_val, LV_ALIGN_RIGHT_MID, -54, 0);
    lv_obj_set_width(lbl_val, 66);
    lv_obj_set_style_text_align(lbl_val, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    s_tctx.lbl = lbl_val;
    update_timeout_label();

    lv_obj_t *btn_inc = lv_button_create(row_to);
    lv_obj_set_size(btn_inc, 36, 36);
    lv_obj_align(btn_inc, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_inc, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_inc, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_inc, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_inc, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_inc, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_inc, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn_inc, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_inc, timeout_inc_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lbl_inc, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_inc, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_inc);

    lv_scr_load(scr);
}
