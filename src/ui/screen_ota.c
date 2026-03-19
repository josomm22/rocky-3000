/*
 * GBWUI — OTA Firmware Update screen (DESIGN.md §5.7)
 *
 * The HTTP server (GET /ota, POST /update) now lives in web_server.c and
 * runs persistently.  This screen just displays the URL and polls the
 * volatile progress vars exposed by web_server.h.
 */

#include <stdio.h>
#include <string.h>
#include "esp_netif.h"
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_ota.h"
#include "screen_settings.h"
#include "screen_wifi.h"
#include "web_server.h"
#include "ota_checker.h"

/* ── UI handles ──────────────────────────────────────────────── */
static lv_obj_t  *s_lbl_status   = NULL;
static lv_obj_t  *s_bar          = NULL;
static lv_timer_t *s_poll        = NULL;
static lv_timer_t *s_reboot_timer = NULL;

/* ═══════════════════════════════════════════════════════════════
 * LVGL timers
 * ═══════════════════════════════════════════════════════════════ */

static void reboot_cb(lv_timer_t *t)
{
    (void)t;
    extern void esp_restart(void);
    esp_restart();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_status) return;

    if (web_server_ota_error) {
        lv_label_set_text(s_lbl_status, "Update failed. Try again.");
        lv_obj_set_style_text_color(s_lbl_status, COL_ERROR,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_timer_pause(s_poll);
        return;
    }

    if (web_server_ota_done && !s_reboot_timer) {
        lv_label_set_text(s_lbl_status,
                          "Update complete \xe2\x80\x94 rebooting\xe2\x80\xa6");
        lv_obj_set_style_text_color(s_lbl_status, COL_ACCENT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        if (s_bar) {
            lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
            lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        }
        lv_timer_pause(s_poll);
        s_reboot_timer = lv_timer_create(reboot_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_reboot_timer, 1);
        return;
    }

    if (web_server_ota_pct > 0 && !web_server_ota_done) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Uploading\xe2\x80\xa6 %d%%", web_server_ota_pct);
        lv_label_set_text(s_lbl_status, buf);
        lv_obj_set_style_text_color(s_lbl_status, COL_ACCENT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        if (s_bar) {
            lv_bar_set_value(s_bar, web_server_ota_pct, LV_ANIM_OFF);
            lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── Screen teardown ────────────────────────────────────────── */

static void scr_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_poll)         { lv_timer_delete(s_poll);         s_poll         = NULL; }
    if (s_reboot_timer) { lv_timer_delete(s_reboot_timer); s_reboot_timer = NULL; }
    s_lbl_status = NULL;
    s_bar        = NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Navigation callbacks
 * ═══════════════════════════════════════════════════════════════ */

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_settings_load();
}

static void goto_wifi_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_wifi_load();
}

/* ═══════════════════════════════════════════════════════════════
 * UI states
 * ═══════════════════════════════════════════════════════════════ */

static void render_no_wifi(lv_obj_t *scr)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl,
                      "WiFi not connected.\n\n"
                      "Connect via Settings \xe2\x86\x92 WiFi first.");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(lbl, SCR_W - 60);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 240, 62);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, goto_wifi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn = lv_label_create(btn);
    lv_label_set_text(lbl_btn, LV_SYMBOL_WIFI "  Connect to WiFi");
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_btn, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_btn);
}

static void render_connected(lv_obj_t *scr, const char *ip)
{
    /* "Open in your browser:" */
    lv_obj_t *lbl_hint = lv_label_create(scr);
    lv_label_set_text(lbl_hint, "Open in your browser:");
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_hint, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_MID, 0, 100);

    /* Large accent URL */
    char url[32];
    snprintf(url, sizeof(url), "http://%s/ota", ip);

    lv_obj_t *lbl_url = lv_label_create(scr);
    lv_label_set_text(lbl_url, url);
    lv_obj_set_style_text_font(lbl_url, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_url, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_url, LV_ALIGN_TOP_MID, 0, 142);

    /* Thin divider */
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, SCR_W - 60, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_set_style_bg_color(div, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Status label */
    s_lbl_status = lv_label_create(scr);
    lv_label_set_text(s_lbl_status, "Waiting for upload\xe2\x80\xa6");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_status, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 230);

    /* Progress bar (hidden until upload starts) */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, SCR_W - 80, 22);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 278);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_bar, COL_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

/* ═══════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════ */

void screen_ota_load(void)
{
    web_server_reset_ota_state();
    ota_checker_recheck(); /* kick off / retry auto cloud check */

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

    /* ── Title ──────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Firmware Update");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    /* ── Header divider ─────────────────────────────────────── */
    lv_obj_t *hdiv = lv_obj_create(scr);
    lv_obj_set_size(hdiv, SCR_W - 20, 1);
    lv_obj_align(hdiv, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(hdiv, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hdiv, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(hdiv, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Body ───────────────────────────────────────────────── */
    if (!screen_wifi_is_connected()) {
        render_no_wifi(scr);
    } else {
        char ip[16] = "?.?.?.?";
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0)
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
        render_connected(scr, ip);
        s_poll = lv_timer_create(poll_cb, 500, NULL);
    }

    lv_scr_load(scr);
}
