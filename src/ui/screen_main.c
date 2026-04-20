/*
 * GBWUI — Main screen (Idle / Grinding / Done states)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_main.h"
#include "screen_settings.h"
#include "screen_wifi.h"
#include "screen_preset_edit.h"
#include "grind_controller.h"
#include "grind_history.h"
#include "ota_checker.h"

/* ── NVS namespace for presets ──────────────────────────────── */
#define PRESET_NVS_NS "app_cfg"

/* ── Preset state (persists across reloads) ─────────────────── */
static float s_weights[PRESET_MAX] = {18.0f, 21.0f};
static int s_count = 2;
static int s_active = 0;

/* ── NVS preset persistence ─────────────────────────────────── */
static void presets_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "p_count", (uint8_t)s_count);
    nvs_set_u8(h, "p_active", (uint8_t)s_active);
    for (int i = 0; i < s_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "p_%d", i);
        uint32_t bits;
        memcpy(&bits, &s_weights[i], sizeof(bits));
        nvs_set_u32(h, key, bits);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── UI object handles (reset on each load) ─────────────────── */
static lv_obj_t   *s_scr          = NULL;
static lv_obj_t   *s_row          = NULL;
static lv_obj_t   *s_pills[PRESET_MAX];
static lv_obj_t   *s_btn_add      = NULL;
static lv_obj_t   *s_sel_frame    = NULL;
static lv_obj_t   *s_btn_grind    = NULL;
static lv_obj_t   *s_lbl_grind    = NULL;
static lv_obj_t   *s_btn_stop     = NULL;
static lv_obj_t   *s_lbl_purge    = NULL;
static lv_obj_t   *s_lbl_wifi     = NULL;
static lv_obj_t   *s_toast_cont   = NULL;

/* ── Timer handles (cleaned up in scr_delete_cb) ────────────── */
static lv_timer_t *s_wifi_poll_timer = NULL;
static lv_timer_t *s_grind_poll      = NULL;
static lv_timer_t *s_done_timer      = NULL;
static lv_timer_t *s_toast_timer     = NULL;
static lv_timer_t *s_update_poll     = NULL;

/* ── OTA update UI (banner + download overlay) ──────────────── */
static lv_obj_t   *s_update_banner   = NULL;
static lv_obj_t   *s_update_overlay  = NULL;
static lv_obj_t   *s_update_bar      = NULL;
static lv_obj_t   *s_update_lbl      = NULL;

/* ── Forward declarations ───────────────────────────────────── */
static void rebuild_preset_row(void);
static void apply_pill_styles(void);
static void position_sel_frame(int idx);

/* ═══════════════════════════════════════════════════════════════
 * Toast
 * ═══════════════════════════════════════════════════════════════ */

static void show_error_toast(const char *msg);  /* forward decl */

static void toast_dismiss_cb(lv_timer_t *t)
{
    (void)t;
    s_toast_timer = NULL;  /* auto-deleted by LVGL (repeat_count=1) */
    if (s_toast_cont) {
        lv_obj_delete(s_toast_cont);
        s_toast_cont = NULL;
    }
}

static void show_grind_toast(float result, float target)
{
    if (!s_scr) return;

    /* Dismiss any existing toast */
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast_cont) {
        lv_obj_delete(s_toast_cont);
        s_toast_cont = NULL;
    }

    float delta    = result - target;
    float new_off  = grind_ctrl_get_offset();

    char buf[96];
    snprintf(buf, sizeof(buf),
             "Done  \xc2\xb7  %.1fg  (%+.1fg)\nOffset \xe2\x86\x92 %.2fg",
             result, delta, new_off);

    lv_obj_t *cont = lv_obj_create(s_scr);
    lv_obj_set_width(cont, SCR_W - 40);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(cont, COL_SUCCESS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cont, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(cont, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(lbl, SCR_W - 40 - 28);

    s_toast_cont  = cont;
    s_toast_timer = lv_timer_create(toast_dismiss_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/* ═══════════════════════════════════════════════════════════════
 * Grind cycle UI helpers
 * ═══════════════════════════════════════════════════════════════ */

static void set_grinding_ui(bool grinding)
{
    if (s_btn_stop) {
        if (grinding)
            lv_obj_remove_flag(s_btn_stop, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_btn_stop, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Called 2 s after grind finishes — resets button back to "GRIND" */
static void done_reset_cb(lv_timer_t *t)
{
    (void)t;
    s_done_timer = NULL;  /* auto-deleted by LVGL */
    grind_ctrl_ack_done();
    if (s_lbl_grind) lv_label_set_text(s_lbl_grind, "GRIND");
    set_grinding_ui(false);
}

static void purge_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    grind_ctrl_purge();
}

/* 100 ms poll — reads grind controller state and updates button label */
static void grind_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_grind) return;  /* screen was deleted */

    /* Purge button label tracks purge state */
    if (s_lbl_purge)
        lv_label_set_text(s_lbl_purge,
                          grind_ctrl_is_purging() ? "PURGING" : "PURGE");

    grind_state_t state = grind_ctrl_get_state();

    if (state == GRIND_IDLE &&
        s_btn_stop && !lv_obj_has_flag(s_btn_stop, LV_OBJ_FLAG_HIDDEN)) {
        /* Tare timed out or start was rejected — revert UI. */
        lv_label_set_text(s_lbl_grind, "GRIND");
        set_grinding_ui(false);
        if (!grind_ctrl_hx711_healthy())
            show_error_toast("Scale not responding\nCheck sensor connection");
    }
    else if (state == GRIND_TARING) {
        lv_label_set_text(s_lbl_grind, "TARE");
        set_grinding_ui(true);
    }
    else if (state == GRIND_RUNNING || state == GRIND_SETTLING || state == GRIND_PULSING) {
        float w = grind_ctrl_get_weight();
        w = (float)((int)(w * 10.0f + (w >= 0 ? 0.5f : -0.5f))) / 10.0f;
        char buf[16];
        if (state == GRIND_PULSING)
            snprintf(buf, sizeof(buf), "~%.1fg", w);
        else
            snprintf(buf, sizeof(buf), "%.1fg", w);
        lv_label_set_text(s_lbl_grind, buf);
        set_grinding_ui(true);
    }
    else if (state == GRIND_DONE && !s_done_timer) {
        /* First poll after grind completes */
        float result = grind_ctrl_get_result();
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1fg", result);
        lv_label_set_text(s_lbl_grind, buf);
        set_grinding_ui(false);

        grind_history_record(
            s_weights[s_active], result,
            grind_ctrl_get_weight_at_cutoff(),
            grind_ctrl_get_weight_before_pulses(),
            grind_ctrl_get_last_flow_rate(),
            grind_ctrl_get_offset(),
            (uint32_t)time(NULL),
            (uint16_t)grind_ctrl_get_grind_ms(),
            (uint8_t)grind_ctrl_get_pulse_count()
        );
        show_grind_toast(result, s_weights[s_active]);

        s_done_timer = lv_timer_create(done_reset_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_done_timer, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Event callbacks
 * ═══════════════════════════════════════════════════════════════ */

static void preset_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_active = idx;
    presets_save_nvs();
    apply_pill_styles();
    position_sel_frame(idx);
    lv_obj_move_foreground(s_sel_frame);
}

static void add_preset_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_preset_edit_load(-1, 18.0f);
}

static void pill_longpress_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED)
        return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    screen_preset_edit_load(idx, s_weights[idx]);
}

static void show_error_toast(const char *msg)
{
    if (!s_scr) return;

    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast_cont) {
        lv_obj_delete(s_toast_cont);
        s_toast_cont = NULL;
    }

    lv_obj_t *cont = lv_obj_create(s_scr);
    lv_obj_set_width(cont, SCR_W - 40);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(cont, COL_ERROR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cont, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(cont, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(lbl, SCR_W - 40 - 28);

    s_toast_cont  = cont;
    s_toast_timer = lv_timer_create(toast_dismiss_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

static void grind_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    if (grind_ctrl_get_state() != GRIND_IDLE)
        return;

    if (!grind_ctrl_hx711_healthy()) {
        show_error_toast("Scale not responding\nCheck sensor connection");
        return;
    }

    grind_ctrl_start(s_weights[s_active]);
    lv_label_set_text(s_lbl_grind, "TARE");
    set_grinding_ui(true);
}

static void stop_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    grind_ctrl_stop();
    /* grind_poll_cb detects DONE and handles the rest */
}

static void wifi_icon_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_wifi)
        return;
    lv_obj_set_style_text_color(s_lbl_wifi,
                                screen_wifi_is_connected() ? COL_ACCENT : COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_install_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ota_checker_apply();
}

static void update_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scr) return;

    ota_check_state_t state = ota_checker_get_state();

    /* Show the "update available" banner once */
    if (state == OTA_CHECK_AVAILABLE && !s_update_banner) {
        char buf[64];
        if (ota_checker_has_binary()) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_UP " Update %s — tap to install",
                     ota_checker_get_version());
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_UP " Update %s available",
                     ota_checker_get_version());
        }

        s_update_banner = lv_button_create(s_scr);
        lv_obj_set_width(s_update_banner, SCR_W - 24);
        lv_obj_set_height(s_update_banner, LV_SIZE_CONTENT);
        lv_obj_align(s_update_banner, LV_ALIGN_TOP_MID, 0, 86);
        lv_obj_set_style_bg_color(s_update_banner, COL_ACCENT,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_update_banner, LV_OPA_20,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_update_banner, LV_OPA_40,
                                LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_update_banner, COL_ACCENT,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_update_banner, 1,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_update_banner, 8,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_ver(s_update_banner, 6,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(s_update_banner, 0,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(s_update_banner, update_install_cb,
                            LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(s_update_banner);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, COL_ACCENT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl);
    }

    /* Full-screen overlay only during an active download */
    if (state == OTA_CHECK_DOWNLOADING || state == OTA_CHECK_DONE) {

        if (!s_update_overlay) {
            /* Hide banner — overlay takes over */
            if (s_update_banner) {
                lv_obj_add_flag(s_update_banner, LV_OBJ_FLAG_HIDDEN);
            }

            s_update_overlay = lv_obj_create(s_scr);
            lv_obj_set_size(s_update_overlay, SCR_W, SCR_H);
            lv_obj_align(s_update_overlay, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(s_update_overlay, COL_BG,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(s_update_overlay, LV_OPA_COVER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(s_update_overlay, 0,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_remove_flag(s_update_overlay, LV_OBJ_FLAG_SCROLLABLE);

            s_update_lbl = lv_label_create(s_update_overlay);
            lv_obj_set_style_text_font(s_update_lbl, &lv_font_montserrat_24,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(s_update_lbl, COL_TEXT,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(s_update_lbl, LV_TEXT_ALIGN_CENTER,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_align(s_update_lbl, LV_ALIGN_CENTER, 0, -40);

            s_update_bar = lv_bar_create(s_update_overlay);
            lv_obj_set_size(s_update_bar, SCR_W - 80, 22);
            lv_obj_align(s_update_bar, LV_ALIGN_CENTER, 0, 10);
            lv_bar_set_range(s_update_bar, 0, 100);
            lv_bar_set_value(s_update_bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_update_bar, COL_SURFACE,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(s_update_bar, COL_ACCENT,
                                      LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(s_update_bar, 11,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(s_update_bar, 11,
                                    LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }

        if (state == OTA_CHECK_DOWNLOADING) {
            int pct = ota_checker_get_progress();
            char buf[56];
            snprintf(buf, sizeof(buf), "Downloading %s\xe2\x80\xa6 %d%%",
                     ota_checker_get_version(), pct);
            lv_label_set_text(s_update_lbl, buf);
            lv_bar_set_value(s_update_bar, pct, LV_ANIM_OFF);
        } else {
            lv_label_set_text(s_update_lbl,
                              "Update complete \xe2\x80\x94 rebooting\xe2\x80\xa6");
            lv_obj_set_style_text_color(s_update_lbl, COL_ACCENT,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_bar_set_value(s_update_bar, 100, LV_ANIM_OFF);
        }
    }

    /* Download error — brief toast, don't block the screen */
    if (state == OTA_CHECK_ERROR && s_update_overlay) {
        lv_obj_delete(s_update_overlay);
        s_update_overlay = NULL;
        s_update_bar     = NULL;
        s_update_lbl     = NULL;
        if (s_update_banner) lv_obj_remove_flag(s_update_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scr_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_poll_timer) { lv_timer_delete(s_wifi_poll_timer); s_wifi_poll_timer = NULL; }
    if (s_grind_poll)      { lv_timer_delete(s_grind_poll);      s_grind_poll      = NULL; }
    if (s_done_timer)      { lv_timer_delete(s_done_timer);      s_done_timer      = NULL; }
    if (s_toast_timer)     { lv_timer_delete(s_toast_timer);     s_toast_timer     = NULL; }
    if (s_update_poll)     { lv_timer_delete(s_update_poll);     s_update_poll     = NULL; }
    s_scr           = NULL;
    s_lbl_wifi      = NULL;
    s_lbl_grind     = NULL;
    s_btn_stop      = NULL;
    s_lbl_purge     = NULL;
    s_toast_cont    = NULL;  /* deleted with the screen */
    s_update_banner  = NULL;
    s_update_overlay = NULL;
    s_update_bar     = NULL;
    s_update_lbl     = NULL;
}

static void settings_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_settings_load();
}

static void wifi_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    screen_wifi_load();
}

/* ── Style helpers ──────────────────────────────────────────── */

static void apply_pill_styles(void)
{
    for (int i = 0; i < s_count; i++)
    {
        if (!s_pills[i])
            continue;
        bool sel = (i == s_active);

        lv_obj_set_style_bg_color(s_pills[i],
                                  sel ? COL_ACCENT : COL_PRESET_BG,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_pills[i],
                                sel ? LV_OPA_20 : LV_OPA_COVER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_pills[i],
                                      sel ? COL_ACCENT : COL_SURFACE,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *lbl = lv_obj_get_child(s_pills[i], 0);
        if (lbl)
        {
            lv_obj_set_style_text_color(lbl,
                                        sel ? COL_ACCENT : COL_TEXT,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void position_sel_frame(int idx)
{
    if (!s_sel_frame || idx < 0 || idx >= s_count)
        return;
    lv_obj_align_to(s_sel_frame, s_pills[idx], LV_ALIGN_CENTER, 0, 0);
}

static void rebuild_preset_row(void)
{
    lv_obj_clean(s_row);
    memset(s_pills, 0, sizeof(s_pills));
    s_btn_add = NULL;

    for (int i = 0; i < s_count; i++)
    {
        lv_obj_t *pill = lv_button_create(s_row);
        lv_obj_set_size(pill, PILL_W, PILL_H);
        lv_obj_set_style_radius(pill, PILL_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(pill, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(pill, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(pill, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(pill, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(pill, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_event_cb(pill, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(pill, pill_longpress_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        char buf[12];
        float w = s_weights[i];
        if (w == (float)(int)w)
            snprintf(buf, sizeof(buf), "%dg", (int)w);
        else
            snprintf(buf, sizeof(buf), "%.1fg", w);

        lv_obj_t *lbl = lv_label_create(pill);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lbl);

        s_pills[i] = pill;
    }

    if (s_count < PRESET_MAX)
    {
        s_btn_add = lv_button_create(s_row);
        lv_obj_set_size(s_btn_add, PILL_H, PILL_H);
        lv_obj_set_style_radius(s_btn_add, PILL_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_btn_add, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_btn_add, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_btn_add, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_btn_add, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(s_btn_add, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(s_btn_add, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_btn_add, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_btn_add, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_event_cb(s_btn_add, add_preset_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *plus = lv_label_create(s_btn_add);
        lv_label_set_text(plus, "+");
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_32,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(plus, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(plus);
    }

    apply_pill_styles();
}

/* ── Public entry point ─────────────────────────────────────── */

void screen_main_load(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, NULL);

    /* ── WiFi status indicator (top-left) ───────────────────── */
    lv_obj_t *btn_wifi = lv_button_create(scr);
    lv_obj_set_size(btn_wifi, 72, 72);
    lv_obj_align(btn_wifi, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_radius(btn_wifi, 36, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_wifi, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_wifi, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_wifi, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_wifi, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_wifi, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_wifi, wifi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_wifi, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_wifi);
    s_lbl_wifi = lbl_wifi;
    s_wifi_poll_timer = lv_timer_create(wifi_icon_poll_cb, 1000, NULL);
    lv_timer_ready(s_wifi_poll_timer);

    /* ── Settings gear (top-right) ─────────────────────────── */
    lv_obj_t *btn_gear = lv_button_create(scr);
    lv_obj_set_size(btn_gear, 108, 108);
    lv_obj_align(btn_gear, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_radius(btn_gear, 54, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_gear, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_gear, COL_SURFACE, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_gear, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_gear, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_gear, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_gear, settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_gear = lv_label_create(btn_gear);
    lv_label_set_text(lbl_gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl_gear, &lv_font_montserrat_48,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_gear, COL_GEAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_gear);

    /* ── Preset pill row ────────────────────────────────────── */
    s_row = lv_obj_create(scr);
    lv_obj_set_height(s_row, PILL_H + 4);
    lv_obj_set_width(s_row, LV_SIZE_CONTENT);
    lv_obj_align(s_row, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_style_bg_opa(s_row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(s_row, PILL_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ── Selection frame (accent ring, layered over active pill) */
    s_sel_frame = lv_obj_create(scr);
    lv_obj_set_size(s_sel_frame, SEL_FRAME_W, SEL_FRAME_H);
    lv_obj_set_style_radius(s_sel_frame, SEL_FRAME_H / 2,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_sel_frame, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_sel_frame, COL_ACCENT,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_sel_frame, 3,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_sel_frame, LV_OPA_COVER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_sel_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_sel_frame, LV_OBJ_FLAG_SCROLLABLE);

    /* ── GRIND button ───────────────────────────────────────── */
    s_btn_grind = lv_button_create(scr);
    lv_obj_set_size(s_btn_grind, 180, 180);
    lv_obj_align(s_btn_grind, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_radius(s_btn_grind, 90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_grind, COL_GRIND_BG,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_grind, COL_GRIND_HL,
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_btn_grind, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_grind, COL_GRIND_HL,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_btn_grind, 2,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_btn_grind, LV_OPA_COVER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_btn_grind, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_btn_grind, grind_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_grind = lv_label_create(s_btn_grind);
    lv_label_set_text(s_lbl_grind, "GRIND");
    lv_obj_set_style_text_font(s_lbl_grind, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_grind, lv_color_white(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(s_lbl_grind, 4,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_lbl_grind);

    /* ── STOP button (hidden until grinding, bottom-right) ──── */
    s_btn_stop = lv_button_create(scr);
    lv_obj_set_size(s_btn_stop, 117, 90);
    lv_obj_align(s_btn_stop, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_radius(s_btn_stop, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_stop, COL_ERROR,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_stop, lv_color_hex(0xc62828),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_btn_stop, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_stop, lv_color_hex(0xc62828),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_btn_stop, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_btn_stop, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_btn_stop, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_btn_stop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_btn_stop, stop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_stop = lv_label_create(s_btn_stop);
    lv_label_set_text(lbl_stop, "STOP");
    lv_obj_set_style_text_font(lbl_stop, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_stop, lv_color_white(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_stop);

    /* ── PURGE button (bottom-left) ─────────────────────────── */
    lv_obj_t *btn_purge = lv_button_create(scr);
    lv_obj_set_size(btn_purge, 117, 90);
    lv_obj_align(btn_purge, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_radius(btn_purge, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_purge, lv_color_hex(0x8b0000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_purge, lv_color_hex(0xc62828),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_purge, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_purge, lv_color_hex(0xc62828),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_purge, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn_purge, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn_purge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_purge, purge_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_purge = lv_label_create(btn_purge);
    lv_label_set_text(lbl_purge, "PURGE");
    s_lbl_purge = lbl_purge;
    lv_obj_set_style_text_font(lbl_purge, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_purge, lv_color_white(),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_purge);

    /* ── Grind state poll timer ─────────────────────────────── */
    s_grind_poll = lv_timer_create(grind_poll_cb, 100, NULL);

    /* ── OTA update check poll (2 s) ────────────────────────── */
    s_update_poll = lv_timer_create(update_poll_cb, 2000, NULL);

    rebuild_preset_row();

    lv_scr_load(scr);

    /* Position sel_frame after layout is resolved */
    lv_obj_update_layout(scr);
    position_sel_frame(s_active);
    lv_obj_move_foreground(s_sel_frame);
}

/* ═══════════════════════════════════════════════════════════════
 * Public preset API
 * ═══════════════════════════════════════════════════════════════ */

void screen_main_preset_init(void)
{
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t cnt = 0;
    if (nvs_get_u8(h, "p_count", &cnt) == ESP_OK && cnt >= 1 && cnt <= PRESET_MAX) {
        int loaded = 0;
        for (int i = 0; i < (int)cnt; i++) {
            char key[8];
            snprintf(key, sizeof(key), "p_%d", i);
            uint32_t bits;
            if (nvs_get_u32(h, key, &bits) == ESP_OK) {
                float w;
                memcpy(&w, &bits, sizeof(w));
                if (w >= 0.1f && w <= 99.9f)
                    s_weights[loaded++] = w;
            }
        }
        if (loaded > 0) s_count = loaded;
    }
    uint8_t active = 0;
    if (nvs_get_u8(h, "p_active", &active) == ESP_OK && (int)active < s_count)
        s_active = (int)active;
    nvs_close(h);
    if (s_active >= s_count) s_active = 0;
}

void screen_main_add_preset(float w)
{
    if (s_count >= PRESET_MAX) return;
    s_weights[s_count++] = w;
    s_active = s_count - 1;
    presets_save_nvs();
}

void screen_main_edit_preset(int idx, float w)
{
    if (idx < 0 || idx >= s_count) return;
    s_weights[idx] = w;
    presets_save_nvs();
}

void screen_main_delete_preset(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    for (int i = idx; i < s_count - 1; i++)
        s_weights[i] = s_weights[i + 1];
    s_count--;
    if (s_count == 0) { s_weights[0] = 18.0f; s_count = 1; } /* always keep 1 */
    if (s_active >= s_count) s_active = s_count - 1;
    presets_save_nvs();
}
