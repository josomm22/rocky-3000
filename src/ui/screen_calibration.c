/*
 * GBWUI — Calibration wizard (3-step, DESIGN.md §5.6)
 *
 * Step 1  Remove all weight → live tare reading → Continue
 * Step 2  Place known weight → enter known value → Continue
 * Step 3  Review: reading / known / factor → Discard | Save
 *
 * Demo mode behaviour:
 *   Step 1 shows 0.00 g (scale is idle, s_weight = 0).
 *   Step 2 shows (known × 0.997) — simulates a ~0.3 % uncalibrated error
 *   so the factor calculation has something realistic to display.
 */

#include <stdio.h>
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_calibration.h"
#include "screen_settings.h"
#include "grind_controller.h"

/* ── Wizard state (reset on each screen_calibration_load) ────── */
static int    s_step        = 1;
static float  s_known_g     = 100.0f;
static float  s_raw_reading = 0.0f;
static float  s_cal_factor  = 1.0f;

/* ── UI handles ─────────────────────────────────────────────── */
static lv_obj_t   *s_content    = NULL;  /* cleared & rebuilt per step  */
static lv_obj_t   *s_lbl_title  = NULL;  /* "Calibration  N of 3"       */
static lv_obj_t   *s_lbl_weight = NULL;  /* live reading (steps 1 & 2)  */
static lv_obj_t   *s_lbl_known  = NULL;  /* known-weight value (step 2) */
static lv_timer_t *s_poll       = NULL;

/* ── Forward declarations ───────────────────────────────────── */
static void render_step1(void);
static void render_step2(void);
static void render_step3(void);

/* ═══════════════════════════════════════════════════════════════
 * Poll timer — updates live weight label in steps 1 & 2
 * ═══════════════════════════════════════════════════════════════ */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_weight || s_step > 2) return;

    float w;
    if (s_step == 1) {
        w = grind_ctrl_get_live_weight();   /* 0.0 in demo (idle) */
    } else {
        /* Step 2: show calibrated reading (what the scale currently displays).
         * Demo: derive from known weight with a fixed -0.3 % bias so the
         * wizard has a non-trivial factor to compute. */
        w = grind_ctrl_is_demo() ? s_known_g * 0.997f
                                 : grind_ctrl_get_live_weight();
    }

    /* Round to 0.1 g to suppress sub-digit jitter on the display */
    w = (float)((int)(w * 10.0f + (w >= 0 ? 0.5f : -0.5f))) / 10.0f;

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f g", w);
    lv_label_set_text(s_lbl_weight, buf);
}

/* ── Screen teardown ────────────────────────────────────────── */

static void scr_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    s_content    = NULL;
    s_lbl_title  = NULL;
    s_lbl_weight = NULL;
    s_lbl_known  = NULL;
}

/* ── Navigation callbacks ───────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_settings_load();
}

static void update_title(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "Calibration  %d of 3", s_step);
    lv_label_set_text(s_lbl_title, buf);
}

static void go_step2(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    grind_ctrl_tare();   /* user confirmed scale is empty — re-zero now */
    s_step = 2;
    update_title();
    s_lbl_weight = NULL;
    lv_obj_clean(s_content);
    render_step2();
}

static void go_step3(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* Capture the *calibrated* reading at this moment.
     * grind_ctrl_get_live_weight() already has the current cal factor baked in
     * (raw × old_cal), so the correct new factor is:
     *   new_cal = old_cal × (known / displayed)            */
    s_raw_reading = grind_ctrl_is_demo() ? s_known_g * 0.997f
                                         : grind_ctrl_get_live_weight();
    if (s_raw_reading < 0.001f) s_raw_reading = 0.001f;  /* guard div/0 */
    s_cal_factor = grind_ctrl_get_cal_factor() * s_known_g / s_raw_reading;

    s_step = 3;
    update_title();
    s_lbl_weight = NULL;
    lv_obj_clean(s_content);
    render_step3();
}

static void discard_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_settings_load();
}

static void save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    grind_ctrl_set_cal_factor(s_cal_factor);
    screen_settings_load();
}

/* ── Known-weight stepper callbacks ─────────────────────────── */

static void update_known_label(void)
{
    if (!s_lbl_known) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f g", s_known_g);
    lv_label_set_text(s_lbl_known, buf);
}

static void known_minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_known_g -= 0.1f;
    if (s_known_g < 0.1f) s_known_g = 0.1f;
    s_known_g = (float)((int)(s_known_g * 10.0f + 0.5f)) / 10.0f;
    update_known_label();
}

static void known_plus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_known_g += 0.1f;
    if (s_known_g > 5000.0f) s_known_g = 5000.0f;
    s_known_g = (float)((int)(s_known_g * 10.0f + 0.5f)) / 10.0f;
    update_known_label();
}

/* ═══════════════════════════════════════════════════════════════
 * Widget helpers
 * ═══════════════════════════════════════════════════════════════ */

/* Framed box used to display the live weight reading */
static lv_obj_t *make_weight_box(lv_obj_t *parent, int y_offset)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 280, 90);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_set_style_bg_color(box, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(box, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static lv_obj_t *make_weight_label_in_box(lv_obj_t *box)
{
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
    return lbl;
}

/* Standard "Continue" button aligned to bottom-right of content */
static void make_continue_btn(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 170, 62);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Continue");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
}

/* Simple ± stepper button */
static lv_obj_t *make_stepper_btn(lv_obj_t *parent, const char *symbol,
                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_set_style_radius(btn, 32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
    return btn;
}

/* ═══════════════════════════════════════════════════════════════
 * Step 1 — Remove weight, tare, Continue
 * ═══════════════════════════════════════════════════════════════ */

static void render_step1(void)
{
    lv_obj_t *instr = lv_label_create(s_content);
    lv_label_set_text(instr,
                      "Remove everything from the scale\n"
                      "and press Continue.");
    lv_obj_set_style_text_font(instr, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(instr, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(instr, SCR_W - 60);
    lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *box = make_weight_box(s_content, 120);
    s_lbl_weight  = make_weight_label_in_box(box);
    lv_label_set_text(s_lbl_weight, "0.00 g");

    make_continue_btn(s_content, go_step2);
}

/* ═══════════════════════════════════════════════════════════════
 * Step 2 — Place known weight, set value, Continue
 * ═══════════════════════════════════════════════════════════════ */

static void render_step2(void)
{
    lv_obj_t *instr = lv_label_create(s_content);
    lv_label_set_text(instr,
                      "Place your calibration weight\n"
                      "on the scale.");
    lv_obj_set_style_text_font(instr, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(instr, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(instr, SCR_W - 60);
    lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 24);

    /* Live raw reading */
    lv_obj_t *box = make_weight_box(s_content, 110);
    s_lbl_weight  = make_weight_label_in_box(box);
    lv_label_set_text(s_lbl_weight, "---");

    /* "Known weight:" heading */
    lv_obj_t *lbl_k = lv_label_create(s_content);
    lv_label_set_text(lbl_k, "Known weight:");
    lv_obj_set_style_text_font(lbl_k, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_k, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_k, LV_ALIGN_TOP_MID, 0, 220);

    /* Stepper row: [−]  value  [+] */
    lv_obj_t *row = lv_obj_create(s_content);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 70);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 258);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(row, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_stepper_btn(row, "-", known_minus_cb);

    s_lbl_known = lv_label_create(row);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f g", s_known_g);
    lv_label_set_text(s_lbl_known, buf);
    lv_obj_set_style_text_font(s_lbl_known, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_known, COL_TEXT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_min_width(s_lbl_known, 150,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_lbl_known, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    make_stepper_btn(row, "+", known_plus_cb);

    make_continue_btn(s_content, go_step3);
}

/* ═══════════════════════════════════════════════════════════════
 * Step 3 — Confirm & save
 * ═══════════════════════════════════════════════════════════════ */

static void render_step3(void)
{
    lv_obj_t *lbl_done = lv_label_create(s_content);
    lv_label_set_text(lbl_done, "Calibration complete.");
    lv_obj_set_style_text_font(lbl_done, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_done, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_done, LV_ALIGN_TOP_MID, 0, 24);

    char info[96];
    snprintf(info, sizeof(info),
             "Reading:    %.2f g\nKnown:      %.1f g\nFactor:     %.4f",
             s_raw_reading, s_known_g, s_cal_factor);

    lv_obj_t *lbl_info = lv_label_create(s_content);
    lv_label_set_text(lbl_info, info);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_info, COL_TEXT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_info, LV_ALIGN_CENTER, 0, -10);

    /* [Discard] — bottom-left, red outline */
    lv_obj_t *btn_dis = lv_button_create(s_content);
    lv_obj_set_size(btn_dis, 160, 62);
    lv_obj_align(btn_dis, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_radius(btn_dis, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_dis, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_dis, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_dis, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_dis, COL_ERROR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_dis, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_dis, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_dis, discard_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dis = lv_label_create(btn_dis);
    lv_label_set_text(lbl_dis, "Discard");
    lv_obj_set_style_text_font(lbl_dis, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_dis, COL_ERROR,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_dis);

    /* [Save] — bottom-right, accent fill */
    lv_obj_t *btn_save = lv_button_create(s_content);
    lv_obj_set_size(btn_save, 160, 62);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_radius(btn_save, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_save, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_save, lv_color_white(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_save, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_save, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_save, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_save, save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_save, COL_BG,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_save);
}

/* ═══════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════ */

void screen_calibration_load(void)
{
    /* Reset wizard state */
    s_step        = 1;
    s_known_g     = 100.0f;
    s_raw_reading = 0.0f;
    s_cal_factor  = 1.0f;

    grind_ctrl_tare();   /* zero the scale on entry */

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, NULL);

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
    lv_obj_set_style_text_color(lbl_back, COL_TEXT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_back);

    /* ── Title label ────────────────────────────────────────── */
    s_lbl_title = lv_label_create(scr);
    lv_label_set_text(s_lbl_title, "Calibration  1 of 3");
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_title, COL_TEXT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 22);

    /* ── Divider ────────────────────────────────────────────── */
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, SCR_W - 20, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(div, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Content container (cleared & rebuilt per step) ─────── */
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, SCR_W, SCR_H - 88);
    lv_obj_align(s_content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    render_step1();

    /* ── Poll timer (200 ms — plenty fast for a static weight display) */
    s_poll = lv_timer_create(poll_cb, 200, NULL);
    lv_timer_ready(s_poll);  /* fire once immediately to show initial reading */

    lv_scr_load(scr);
}
