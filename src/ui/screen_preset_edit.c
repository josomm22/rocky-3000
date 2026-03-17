/*
 * GBWUI — Preset add/edit screen
 *
 * Add mode  (idx == -1): picker starts at initial_weight, Save appends a new preset.
 * Edit mode (idx >= 0) : picker starts at the preset's current value,
 *                        Save updates it, Delete removes it.
 *
 * Layout (640 × 480):
 *   Header  : back button + title + divider  (y 0..80)
 *   Value   : large "XX.X g" label           (center y ≈ 170)
 *   Steppers: [−1g] [−.1g] [+.1g] [+1g]    (center y ≈ 270)
 *   Actions : SAVE [+ DELETE in edit mode]   (bottom, y ≈ 390)
 */

#include <stdio.h>
#include <math.h>
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_preset_edit.h"
#include "screen_main.h"

/* ── Screen state ───────────────────────────────────────────── */
static int       s_edit_idx = -1;
static float     s_value    = 18.0f;
static lv_obj_t *s_val_lbl  = NULL;

/* ── Value helpers ──────────────────────────────────────────── */

static void update_val_lbl(void)
{
    if (!s_val_lbl) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f g", s_value);
    lv_label_set_text(s_val_lbl, buf);
}

/* delta_units: integer multiples of 0.1 g  (e.g. -10 = -1 g, +1 = +0.1 g) */
static void apply_step(int delta_units)
{
    int val_units = (int)roundf(s_value * 10.0f) + delta_units;
    if (val_units < 1)   val_units = 1;   /* min 0.1 g */
    if (val_units > 999) val_units = 999; /* max 99.9 g */
    s_value = val_units / 10.0f;
    update_val_lbl();
}

/* ── Event callbacks ────────────────────────────────────────── */

static void step_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    apply_step(delta);
}

static void save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_edit_idx < 0)
        screen_main_add_preset(s_value);
    else
        screen_main_edit_preset(s_edit_idx, s_value);
    screen_main_load();
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_main_delete_preset(s_edit_idx);
    screen_main_load();
}

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_main_load();
}

/* ── Step-button factory ────────────────────────────────────── */

static void make_step_btn(lv_obj_t *parent, const char *label,
                          int delta_units, bool accent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 110, 68);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn,
                                  accent ? COL_ACCENT : COL_TEXT_DIM,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)delta_units);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl,
                                accent ? COL_ACCENT : COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
}

/* ── Public entry point ─────────────────────────────────────── */

void screen_preset_edit_load(int idx, float initial_weight)
{
    s_edit_idx = idx;
    s_value    = initial_weight;
    s_val_lbl  = NULL;

    bool edit_mode = (idx >= 0);

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
    lv_label_set_text(title, edit_mode ? "Edit Preset" : "Add Preset");
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

    /* ── Value label ────────────────────────────────────────── */
    lv_obj_t *val_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_48,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(val_lbl, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, -70);
    s_val_lbl = val_lbl;
    update_val_lbl();

    /* ── Stepper row ────────────────────────────────────────── */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 68);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_step_btn(row, "-1g",  -10, true);
    make_step_btn(row, "-.1g",  -1, false);
    make_step_btn(row, "+.1g",  +1, false);
    make_step_btn(row, "+1g",  +10, true);

    /* ── Action buttons ─────────────────────────────────────── */
    if (edit_mode)
    {
        /* SAVE (left) */
        lv_obj_t *btn_save = lv_button_create(scr);
        lv_obj_set_size(btn_save, 285, 75);
        lv_obj_align(btn_save, LV_ALIGN_BOTTOM_LEFT, 20, -20);
        lv_obj_set_style_radius(btn_save, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_save, COL_GRIND_BG,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_save, COL_GRIND_HL,
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn_save, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn_save, COL_GRIND_HL,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn_save, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(btn_save, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn_save, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn_save, save_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_save = lv_label_create(btn_save);
        lv_label_set_text(lbl_save, "SAVE");
        lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_32,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(lbl_save, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl_save);

        /* DELETE (right) */
        lv_obj_t *btn_del = lv_button_create(scr);
        lv_obj_set_size(btn_del, 285, 75);
        lv_obj_align(btn_del, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
        lv_obj_set_style_radius(btn_del, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_del, COL_ERROR,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_del, lv_color_hex(0xc62828),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn_del, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn_del, lv_color_hex(0xc62828),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn_del, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(btn_del, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn_del, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn_del, delete_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_del = lv_label_create(btn_del);
        lv_label_set_text(lbl_del, "DELETE");
        lv_obj_set_style_text_font(lbl_del, &lv_font_montserrat_32,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(lbl_del, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl_del);
    }
    else
    {
        /* SAVE (full width) */
        lv_obj_t *btn_save = lv_button_create(scr);
        lv_obj_set_size(btn_save, SCR_W - 40, 75);
        lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_radius(btn_save, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_save, COL_GRIND_BG,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn_save, COL_GRIND_HL,
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn_save, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn_save, COL_GRIND_HL,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn_save, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(btn_save, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn_save, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn_save, save_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_save = lv_label_create(btn_save);
        lv_label_set_text(lbl_save, "SAVE");
        lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_32,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(lbl_save, lv_color_white(),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl_save);
    }

    lv_scr_load(scr);
}
