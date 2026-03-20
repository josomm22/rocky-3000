/*
 * GBWUI — WiFi screen
 *
 * The on-device UI is intentionally minimal — no on-screen keyboard:
 *
 *   Row 1: Current connection status (SSID or "Not connected")
 *   Row 2: AP Setup Mode toggle  (OFF → ON starts the soft-AP + HTTP portal)
 *
 * When AP mode is ON the user connects their phone/laptop to the
 * "GBWUI-Setup" network and opens http://192.168.4.1 in a browser.
 * The portal page lists nearby networks, accepts a password, and
 * posts back the credentials. On success the ESP switches back to STA
 * mode, connects, saves creds to NVS, and the UI updates automatically.
 *
 * NVS: namespace "wifi_cfg", keys "ssid" / "password"
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#include "lvgl.h"
#include "ui_palette.h"
#include "screen_wifi.h"
#include "screen_settings.h"
#include "wifi_portal.h"
#include "secrets.h"
#include "ota_checker.h"

static const char *TAG = "screen_wifi";

#define HEADER_H 80
#define CONTENT_H (SCR_H - HEADER_H)
#define ROW_H 80
#define ROW_GAP 12
#define ROW_W (SCR_W - 40)
#define NVS_NS "wifi_cfg"

/* ── Module state ─────────────────────────────────────────────── */
static bool s_wifi_inited = false;
static bool s_ap_mode = false;

/* Set by event task; read by 500 ms poll timer */
static volatile bool s_got_ip = false;
static volatile bool s_disconnected = false;

static char s_conn_ssid[33]; /* SSID we are connected to, empty if none */

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_content = NULL;
static lv_timer_t *s_poll_timer = NULL;

/* Forward declarations */
static void refresh_ui(void);

/* ── NVS helpers ──────────────────────────────────────────────── */

static void nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "password", pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ── WiFi event handlers ──────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_disconnected = true;
        /* Auto-retry unless AP/portal mode is active */
        if (!s_ap_mode && s_wifi_inited)
            esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP)
    {
        s_got_ip = true;
        /* Retry OTA check now that we have connectivity.
         * Use recheck so a prior NO_UPDATE/ERROR result is cleared — the
         * earlier attempt may have failed due to no IP yet. */
        ota_checker_recheck();
    }
}

/* ── One-time WiFi stack init ─────────────────────────────────── */

static void wifi_init_once(void)
{
    if (s_wifi_inited)
        return;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_inited = true;
}

/* ── Boot-time auto-connect ──────────────────────────────────────── */

void wifi_autoconnect(void)
{
    wifi_init_once();

    nvs_handle_t h;
    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);

    bool have_creds = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK)
    {
        have_creds =
            (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0] != '\0');
        nvs_get_str(h, "password", password, &pass_len);
        nvs_close(h);
    }

    /* Fall back to compile-time defaults from secrets.h if NVS is empty */
    if (!have_creds && DEFAULT_WIFI_SSID[0] != '\0')
    {
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, DEFAULT_WIFI_PASSWORD, sizeof(password) - 1);
        have_creds = true;
    }

    if (!have_creds)
        return;

    strncpy(s_conn_ssid, ssid, sizeof(s_conn_ssid) - 1);
    s_conn_ssid[sizeof(s_conn_ssid) - 1] = '\0';

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode =
        (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Auto-connecting to: %s", ssid);
}

/* ── AP mode start / stop ─────────────────────────────────────── */

static void ap_mode_start(void)
{
    if (s_ap_mode)
        return;

    /* Stop any in-progress STA connection */
    esp_wifi_disconnect();

    wifi_portal_start();
    s_ap_mode = true;
    ESP_LOGI(TAG, "AP portal started");
}

static void ap_mode_stop(void)
{
    if (!s_ap_mode)
        return;

    wifi_portal_stop();
    s_ap_mode = false;
    ESP_LOGI(TAG, "AP portal stopped");

    /* Reconnect to previously saved network (if any) */
    nvs_handle_t h;
    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK)
    {
        bool have = (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0] != '\0');
        nvs_get_str(h, "password", password, &pass_len);
        nvs_close(h);
        if (have)
        {
            wifi_config_t sta_cfg = {0};
            strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
            strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
            sta_cfg.sta.threshold.authmode =
                (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
            esp_wifi_connect();
            ESP_LOGI(TAG, "Reconnecting to saved network: %s", ssid);
        }
    }
}

/* Called by portal after credentials submitted — switch to STA and connect */
void screen_wifi_connect_from_portal(const char *ssid, const char *password)
{
    ap_mode_stop();

    strncpy(s_conn_ssid, ssid, sizeof(s_conn_ssid) - 1);
    s_conn_ssid[sizeof(s_conn_ssid) - 1] = '\0';

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode =
        (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    /* Save optimistically; will be cleared on persistent disconnect */
    nvs_save_creds(ssid, password);
}

/* ── UI helpers ───────────────────────────────────────────────── */

static lv_obj_t *make_row(lv_obj_t *parent, int row_index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0,
                 12 + row_index * (ROW_H + ROW_GAP));
    lv_obj_set_style_bg_color(row, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void toggle_ap_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED))
    {
        ap_mode_start();
    }
    else
    {
        ap_mode_stop();
    }
    refresh_ui();
}

static void refresh_ui(void)
{
    lv_obj_clean(s_content);

    /* ── Row 0: current connection ── */
    lv_obj_t *row0 = make_row(s_content, 0);

    wifi_ap_record_t ap_info;
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    lv_obj_t *lbl_prefix = lv_label_create(row0);
    lv_label_set_text(lbl_prefix, "Network:");
    lv_obj_set_style_text_font(lbl_prefix, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_prefix, COL_TEXT_DIM, 0);
    lv_obj_align(lbl_prefix, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *lbl_ssid = lv_label_create(row0);
    if (connected)
    {
        strncpy(s_conn_ssid, (char *)ap_info.ssid, sizeof(s_conn_ssid) - 1);
        lv_label_set_text(lbl_ssid, s_conn_ssid);
        lv_obj_set_style_text_color(lbl_ssid, COL_ACCENT, 0);
    }
    else
    {
        lv_label_set_text(lbl_ssid, "Not connected");
        lv_obj_set_style_text_color(lbl_ssid, COL_TEXT_DIM, 0);
    }
    lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(lbl_ssid, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_ssid, ROW_W - 160);
    lv_obj_align_to(lbl_ssid, lbl_prefix, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    /* ── Row 1: AP Setup Mode toggle ── */
    lv_obj_t *row1 = make_row(s_content, 1);

    lv_obj_t *lbl_title1 = lv_label_create(row1);
    lv_label_set_text(lbl_title1, "AP Setup Mode");
    lv_obj_set_style_text_font(lbl_title1, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_title1, COL_TEXT, 0);
    lv_obj_align(lbl_title1, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row1);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, COL_PRESET_BG, 0);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_SURFACE, LV_PART_INDICATOR);
    if (s_ap_mode)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, toggle_ap_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Row 2: instruction card (visible only when AP is on) ── */
    if (s_ap_mode)
    {
        lv_obj_t *row2 = make_row(s_content, 2);
        lv_obj_set_height(row2, 130);
        lv_obj_set_style_bg_color(row2, COL_PRESET_BG, 0);

        lv_obj_t *icon = lv_label_create(row2);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(icon, COL_ACCENT, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *inst = lv_label_create(row2);
        lv_label_set_text(inst,
                          "1. Join  \"GBWUI-Setup\"  on your\n"
                          "   phone or laptop\n"
                          "2. Open  http://192.168.4.1\n"
                          "3. Select your network & enter\n"
                          "   the password");
        lv_obj_set_style_text_font(inst, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(inst, COL_TEXT, 0);
        lv_label_set_long_mode(inst, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(inst, ROW_W - 90);
        lv_obj_align(inst, LV_ALIGN_LEFT_MID, 72, 0);
    }
}

/* ── Poll timer (500 ms) ──────────────────────────────────────── */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scr)
        return;

    /* Portal submitted credentials → connecting */
    if (wifi_portal_has_result())
    {
        char ssid[33], pass[65];
        wifi_portal_get_result(ssid, sizeof(ssid), pass, sizeof(pass));
        screen_wifi_connect_from_portal(ssid, pass);
        refresh_ui();
        return;
    }

    if (s_got_ip)
    {
        s_got_ip = false;
        refresh_ui();
        return;
    }

    if (s_disconnected)
    {
        s_disconnected = false;
        s_conn_ssid[0] = '\0';
        refresh_ui();
    }
}

/* ── Back button ──────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    if (s_poll_timer)
    {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    /* Leave AP mode running if the user enabled it and navigates back;
     * they can toggle it off from the same screen on re-entry. */
    s_scr = NULL;
    screen_settings_load();
}

/* ── Public entry point ───────────────────────────────────────── */

void screen_wifi_load(void)
{
    s_got_ip = false;
    s_disconnected = false;

    wifi_init_once();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *btn_back = lv_button_create(s_scr);
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

    /* Title */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_scr);
    lv_obj_set_size(div, SCR_W - 20, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_color(div, COL_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Content container */
    s_content = lv_obj_create(s_scr);
    lv_obj_set_size(s_content, SCR_W, CONTENT_H);
    lv_obj_align(s_content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    refresh_ui();

    s_poll_timer = lv_timer_create(poll_cb, 500, NULL);

    lv_scr_load(s_scr);
}

/* ── Public helpers ───────────────────────────────────────────── */

bool screen_wifi_is_connected(void)
{
    wifi_ap_record_t info;
    return (s_wifi_inited && esp_wifi_sta_get_ap_info(&info) == ESP_OK);
}
