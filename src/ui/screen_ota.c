/*
 * GBWUI — OTA Firmware Update screen (DESIGN.md §5.7)
 *
 * When WiFi is connected the screen:
 *   1. Starts an ESP-IDF HTTP server on port 80
 *   2. Displays the device IP so the user can open it in a browser
 *   3. Serves a single-page upload form (GET /)
 *   4. Accepts the raw .bin POST body (POST /update)
 *   5. Writes firmware via esp_ota_*, reboots on success
 *
 * Progress is communicated from the httpd worker thread to the LVGL
 * timer via volatile variables (32-bit aligned — safe on Xtensa).
 *
 * The HTTP server is stopped cleanly whenever the screen is left.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "lvgl.h"
#include "ui_palette.h"
#include "screen_ota.h"
#include "screen_settings.h"
#include "screen_wifi.h"

/* ── OTA receive buffer (BSS — not on handler task stack) ────── */
#define OTA_BUF_SIZE 4096
static char s_ota_buf[OTA_BUF_SIZE];

/* ── Progress: written by httpd thread, read by LVGL timer ────── */
static volatile int  s_pct   = 0;     /* 0-100                  */
static volatile bool s_done  = false; /* upload + swap complete  */
static volatile bool s_error = false; /* any OTA error           */

/* ── Server & UI handles ─────────────────────────────────────── */
static httpd_handle_t s_server       = NULL;
static lv_obj_t      *s_lbl_status   = NULL;
static lv_obj_t      *s_bar          = NULL;
static lv_timer_t    *s_poll         = NULL;
static lv_timer_t    *s_reboot_timer = NULL;

/* ═══════════════════════════════════════════════════════════════
 * Embedded upload page
 * ═══════════════════════════════════════════════════════════════ */

static const char s_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#111118;color:#e8e8f0;font-family:sans-serif;"
         "display:flex;align-items:center;justify-content:center;"
         "min-height:100vh;padding:16px}"
    ".card{background:#1e1e2e;border-radius:16px;padding:32px 28px;"
           "width:100%;max-width:480px;text-align:center}"
    "h1{color:#4fc3f7;font-size:1.5rem;margin-bottom:14px}"
    "p{color:#9e9eb0;margin-bottom:20px;line-height:1.5}"
    ".fb{display:inline-block;color:#4fc3f7;border:2px solid #4fc3f7;"
        "border-radius:8px;padding:10px 24px;cursor:pointer;margin-bottom:8px}"
    ".fb:hover{background:#25253a}"
    "input[type=file]{display:none}"
    "#fn{color:#666680;font-size:.85rem;margin-bottom:16px;min-height:18px}"
    "button{background:#4fc3f7;color:#111118;border:none;border-radius:8px;"
           "padding:12px 0;font-size:1rem;font-weight:bold;cursor:pointer;width:100%}"
    "button:hover{background:#81d4fa}"
    "button:disabled{background:#25253a;color:#555570;cursor:not-allowed}"
    "#pw{display:none;margin-top:20px}"
    "progress{width:100%;height:18px;border-radius:9px;overflow:hidden;appearance:none}"
    "progress::-webkit-progress-bar{background:#25253a;border-radius:9px}"
    "progress::-webkit-progress-value{background:#4fc3f7;border-radius:9px}"
    "#st{margin-top:14px;color:#4fc3f7;font-size:.95rem;min-height:22px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>&#128640; Firmware Update</h1>"
    "<p>Select a <strong>.bin</strong> firmware file then tap Upload.</p>"
    "<label class='fb' for='f'>&#128193; Choose .bin</label>"
    "<input type='file' id='f' accept='.bin'"
           " onchange=\"document.getElementById('fn').innerText="
                       "this.files[0]?this.files[0].name:''\">"
    "<div id='fn'></div>"
    "<button id='btn' onclick='go()'>Upload</button>"
    "<div id='pw'><progress id='bar' value='0' max='100'></progress></div>"
    "<div id='st'></div>"
    "</div>"
    "<script>"
    "function go(){"
      "var f=document.getElementById('f').files[0];"
      "if(!f){alert('Select a .bin file first');return;}"
      "document.getElementById('btn').disabled=true;"
      "document.getElementById('pw').style.display='block';"
      "document.getElementById('st').innerText='Uploading\u2026';"
      "var x=new XMLHttpRequest();"
      "x.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
          "var p=Math.round(e.loaded/e.total*100);"
          "document.getElementById('bar').value=p;"
          "document.getElementById('st').innerText='Uploading\u2026 '+p+'%';}};"
      "x.onload=function(){"
        "document.getElementById('bar').value=100;"
        "if(x.status===200){"
          "document.getElementById('st').innerText="
            "'\u2713 Update complete \u2014 rebooting\u2026';"
        "}else{"
          "document.getElementById('st').innerText='\u26a0 Failed: '+x.responseText;"
          "document.getElementById('btn').disabled=false;}};"
      "x.onerror=function(){"
        "document.getElementById('st').innerText='\u26a0 Network error';"
        "document.getElementById('btn').disabled=false;};"
      "x.open('POST','/update');"
      "x.send(f);}"
    "</script></body></html>";

/* ═══════════════════════════════════════════════════════════════
 * HTTP handlers
 * ═══════════════════════════════════════════════════════════════ */

static esp_err_t handle_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html, (ssize_t)strlen(s_html));
    return ESP_OK;
}

static esp_err_t handle_post_update(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition found");
        s_error = true;
        return ESP_FAIL;
    }

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        s_error = true;
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int want = total - received;
        if (want > OTA_BUF_SIZE) want = OTA_BUF_SIZE;

        int n = httpd_req_recv(req, s_ota_buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            esp_ota_abort(ota);
            s_error = true;
            return ESP_FAIL;
        }

        if (esp_ota_write(ota, s_ota_buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Flash write failed");
            s_error = true;
            return ESP_FAIL;
        }

        received += n;
        s_pct = received * 100 / total;
    }

    if (esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA end failed (corrupt image?)");
        s_error = true;
        return ESP_FAIL;
    }

    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Set boot partition failed");
        s_error = true;
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    s_done = true;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * Server lifecycle
 * ═══════════════════════════════════════════════════════════════ */

static void start_server(void)
{
    if (s_server) return;

    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.recv_wait_timeout = 60;   /* seconds — allow slow uploads */
    cfg.send_wait_timeout = 10;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return;
    }

    static const httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_get_root,
    };
    static const httpd_uri_t update_uri = {
        .uri     = "/update",
        .method  = HTTP_POST,
        .handler = handle_post_update,
    };
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &update_uri);
}

static void stop_server(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * LVGL timers
 * ═══════════════════════════════════════════════════════════════ */

static void reboot_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_status) return;

    if (s_error) {
        lv_label_set_text(s_lbl_status, "Update failed. Try again.");
        lv_obj_set_style_text_color(s_lbl_status, COL_ERROR,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_timer_pause(s_poll);
        return;
    }

    if (s_done && !s_reboot_timer) {
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

    if (s_pct > 0 && !s_done) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Uploading\xe2\x80\xa6 %d%%", s_pct);
        lv_label_set_text(s_lbl_status, buf);
        lv_obj_set_style_text_color(s_lbl_status, COL_ACCENT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        if (s_bar) {
            lv_bar_set_value(s_bar, s_pct, LV_ANIM_OFF);
            lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── Screen teardown ────────────────────────────────────────── */

static void scr_delete_cb(lv_event_t *e)
{
    (void)e;
    stop_server();
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
    snprintf(url, sizeof(url), "http://%s", ip);

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

    /* Status */
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
    /* Reset progress state from any previous visit */
    s_pct   = 0;
    s_done  = false;
    s_error = false;

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
        start_server();
        s_poll = lv_timer_create(poll_cb, 500, NULL);
    }

    lv_scr_load(scr);
}
