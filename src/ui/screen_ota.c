/*
 * GBWUI — OTA Firmware Update screen (DESIGN.md §5.7)
 *
 * The HTTP server (GET /ota, POST /update) now lives in web_server.c and
 * runs persistently.  This screen just displays the URL and polls the
 * volatile progress vars exposed by web_server.h.
 */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_ota.h"
#include "screen_settings.h"
#include "screen_wifi.h"
#include "web_server.h"
#include "ota_checker.h"
#include "version.h"

/* ── UI handles ──────────────────────────────────────────────── */
static lv_obj_t  *s_lbl_status   = NULL;
static lv_obj_t  *s_bar          = NULL;
static lv_obj_t  *s_lbl_cloud    = NULL;   /* cloud check status text  */
static lv_obj_t  *s_btn_check    = NULL;   /* "Check Now" button       */
static lv_obj_t  *s_lbl_btn      = NULL;   /* label inside that button */
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

/* Update cloud-check status label + button from current ota state */
static void refresh_cloud_ui(void)
{
    if (!s_lbl_cloud || !s_btn_check || !s_lbl_btn) return;

    ota_check_state_t cs = ota_checker_get_state();
    const char *txt  = NULL;
    lv_color_t  col  = COL_TEXT_DIM;
    bool        busy = false;

    switch (cs) {
        case OTA_CHECK_IDLE:
        case OTA_CHECK_CHECKING:
            txt  = "Checking GitHub\xe2\x80\xa6";
            busy = true;
            break;
        case OTA_CHECK_NO_UPDATE:
            txt = "Up to date";
            break;
        case OTA_CHECK_AVAILABLE: {
            static char avail_buf[40];
            snprintf(avail_buf, sizeof(avail_buf),
                     "%s available", ota_checker_get_version());
            txt = avail_buf;
            col = COL_ACCENT;
            break;
        }
        case OTA_CHECK_DOWNLOADING:
        case OTA_CHECK_DONE:
            txt  = "Installing update\xe2\x80\xa6";
            col  = COL_ACCENT;
            busy = true;
            break;
        case OTA_CHECK_ERROR: {
            static char err_buf[64];
            int code = ota_checker_get_http_status();
            if (code > 0) {
                snprintf(err_buf, sizeof(err_buf), "HTTP %d", code);
            } else {
                esp_err_t e = (esp_err_t)ota_checker_get_open_err();
                snprintf(err_buf, sizeof(err_buf), "%s", esp_err_to_name(e));
            }
            txt = err_buf;
            col = COL_ERROR;
            break;
        }
    }

    if (txt) {
        lv_label_set_text(s_lbl_cloud, txt);
        lv_obj_set_style_text_color(s_lbl_cloud, col,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_label_set_text(s_lbl_btn, busy ? "Checking\xe2\x80\xa6" : "Check Now");
    if (busy)
        lv_obj_add_state(s_btn_check, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(s_btn_check, LV_STATE_DISABLED);
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

    refresh_cloud_ui();
}

static void check_now_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ota_checker_force_check();
    refresh_cloud_ui();
}

/* ── Screen teardown ────────────────────────────────────────── */

static void scr_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_poll)         { lv_timer_delete(s_poll);         s_poll         = NULL; }
    if (s_reboot_timer) { lv_timer_delete(s_reboot_timer); s_reboot_timer = NULL; }
    s_lbl_status = NULL;
    s_bar        = NULL;
    s_lbl_cloud  = NULL;
    s_btn_check  = NULL;
    s_lbl_btn    = NULL;
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
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 240, 62);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 74);
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
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_MID, 0, 128);

    /* Large accent URL */
    char url[32];
    snprintf(url, sizeof(url), "http://%s/ota", ip);

    lv_obj_t *lbl_url = lv_label_create(scr);
    lv_label_set_text(lbl_url, url);
    lv_obj_set_style_text_font(lbl_url, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_url, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_url, LV_ALIGN_TOP_MID, 0, 170);

    /* Thin divider */
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, SCR_W - 60, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 236);
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
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 258);

    /* Progress bar (hidden until upload starts) */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, SCR_W - 80, 22);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 306);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_bar, COL_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    /* ── Cloud update check section ─────────────────────────── */
    lv_obj_t *cdiv = lv_obj_create(scr);
    lv_obj_set_size(cdiv, SCR_W - 60, 1);
    lv_obj_align(cdiv, LV_ALIGN_TOP_MID, 0, 344);
    lv_obj_set_style_bg_color(cdiv, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cdiv, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cdiv, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(cdiv, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_lbl_cloud = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_cloud, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_lbl_cloud, LV_ALIGN_TOP_MID, 0, 358);

    s_btn_check = lv_button_create(scr);
    lv_obj_set_size(s_btn_check, SCR_W - 80, 52);
    lv_obj_align(s_btn_check, LV_ALIGN_TOP_MID, 0, 404);
    lv_obj_set_style_radius(s_btn_check, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_check, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_check, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_btn_check, COL_PRESET_BG, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(s_btn_check, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_check, COL_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_btn_check, COL_TEXT_DIM, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_width(s_btn_check, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_btn_check, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_btn_check, check_now_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_btn = lv_label_create(s_btn_check);
    lv_obj_set_style_text_font(s_lbl_btn, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_btn, COL_ACCENT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_btn, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_center(s_lbl_btn);

    refresh_cloud_ui();   /* set initial text from current check state */
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

    /* ── Current version ────────────────────────────────────── */
    lv_obj_t *lbl_ver = lv_label_create(scr);
    lv_label_set_text(lbl_ver, "Current: " APP_VERSION_DISPLAY);
    lv_obj_set_style_text_font(lbl_ver, &lv_font_montserrat_24,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_ver, COL_TEXT_DIM,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(lbl_ver, LV_ALIGN_TOP_MID, 0, 82);

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
