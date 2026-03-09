/*
 * wifi_portal.c — Soft-AP + HTTP captive portal
 *
 * Brings up a "GBWUI-Setup" access point (192.168.4.1 / 192.168.4.0/24)
 * and serves a web page where the user can:
 *   1. See nearby networks (ESP scans after AP starts)
 *   2. Select one and type the password
 *   3. POST the credentials back
 *
 * Everything runs in the ESP-IDF HTTP server task.  After a successful
 * POST, a flag + pending credentials are stored so the LVGL poll timer
 * can pick them up safely on the main core.
 *
 * Security note: the AP has no password and the HTTP channel is
 * unencrypted.  This is intentional for a local-only setup flow;
 * credentials are never exposed on the internet.
 */

#include "wifi_portal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_portal";

#define AP_SSID "GBWUI-Setup"
#define AP_CHAN 1
#define AP_MAX_STA 4
#define SCAN_MAX 20

/* ── State ────────────────────────────────────────────────────── */
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_server = NULL;
static bool s_running = false;

static SemaphoreHandle_t s_result_mutex = NULL;
static bool s_has_result = false;
static char s_result_ssid[33];
static char s_result_pass[65];

static wifi_ap_record_t s_scan_list[SCAN_MAX];
static uint16_t s_scan_count = 0;

/* ── HTML page (stored in flash via string literal) ───────────── */

/*
 * The page is a self-contained single-file HTML/CSS/JS document.
 * It renders a list of scanned networks as <button> elements and
 * shows a password field on selection.  Submission is a plain
 * HTML form POST to /connect (application/x-www-form-urlencoded).
 * No frameworks or CDN dependencies — works completely offline.
 */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html>"
    "<html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>GBWUI WiFi Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#111118;color:#e8e8f0;"
    "display:flex;justify-content:center;padding:24px 12px}"
    ".card{background:#1e1e2e;border-radius:16px;padding:24px;width:100%;"
    "max-width:480px}"
    "h1{font-size:1.4rem;margin-bottom:4px;color:#4fc3f7}"
    ".sub{color:#666680;font-size:.85rem;margin-bottom:20px}"
    ".net-list{display:flex;flex-direction:column;gap:8px;margin-bottom:20px}"
    ".net{background:#25253a;border:2px solid transparent;border-radius:10px;"
    "padding:14px 16px;cursor:pointer;display:flex;align-items:center;"
    "gap:12px;transition:border-color .15s}"
    ".net:hover,.net.sel{border-color:#4fc3f7}"
    ".bars{display:flex;align-items:flex-end;gap:3px;height:18px}"
    ".bar{width:4px;border-radius:2px;background:#25253a}"
    ".bar.lit{background:#4fc3f7}"
    ".ssid{flex:1;font-size:1rem}"
    ".lock{font-size:.75rem;color:#666680}"
    "#pwd-section{display:none;flex-direction:column;gap:12px}"
    "#pwd-section.show{display:flex}"
    "label{font-size:.85rem;color:#666680}"
    "input[type=password]{width:100%;padding:12px 14px;border-radius:8px;"
    "border:2px solid #25253a;background:#111118;color:#e8e8f0;"
    "font-size:1rem;outline:none}"
    "input[type=password]:focus{border-color:#4fc3f7}"
    "button.connect{background:#4fc3f7;color:#111118;border:none;"
    "border-radius:10px;padding:14px;font-size:1rem;font-weight:700;"
    "cursor:pointer;width:100%}"
    "button.connect:hover{background:#81d4fa}"
    ".status{margin-top:16px;padding:12px;border-radius:8px;"
    "text-align:center;display:none}"
    ".status.ok{background:#1b5e20;display:block}"
    ".status.err{background:#b71c1c;display:block}"
    "</style></head><body><div class='card'>"
    "<h1>&#x1F4F6; GBWUI WiFi Setup</h1>"
    "<p class='sub'>Select your network, enter the password, then tap Connect.</p>"
    "<div class='net-list' id='nets'>";

static const char PAGE_TAIL[] =
    "</div>"
    "<form id='frm' method='POST' action='/connect'>"
    "<input type='hidden' id='fssid' name='ssid' value=''>"
    "<div id='pwd-section'>"
    "  <label for='fpwd'>Password for <strong id='sel-name'></strong></label>"
    "  <input type='password' id='fpwd' name='password' autocomplete='current-password'>"
    "  <button type='submit' class='connect'>Connect</button>"
    "</div>"
    "</form>"
    "<div class='status' id='st'></div>"
    "<script>"
    "const nets=document.querySelectorAll('.net');"
    "const ps=document.getElementById('pwd-section');"
    "const fi=document.getElementById('fssid');"
    "const sn=document.getElementById('sel-name');"
    "nets.forEach(n=>n.addEventListener('click',()=>{"
    "  nets.forEach(x=>x.classList.remove('sel'));"
    "  n.classList.add('sel');"
    "  fi.value=n.dataset.ssid;"
    "  sn.textContent=n.dataset.ssid;"
    "  ps.classList.add('show');"
    "  document.getElementById('fpwd').focus();"
    "}));"
    "document.getElementById('frm').addEventListener('submit',async e=>{"
    "  e.preventDefault();"
    "  const st=document.getElementById('st');"
    "  st.className='status';st.textContent='';"
    "  const fd=new FormData(e.target);"
    "  const r=await fetch('/connect',{method:'POST',body:new URLSearchParams(fd)});"
    "  const t=await r.text();"
    "  if(r.ok){st.className='status ok';st.textContent=t;}"
    "  else{st.className='status err';st.textContent=t;}"
    "});"
    "</script>"
    "</div></body></html>";

/* ── URL-decode (in-place) ────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_len; si++)
    {
        if (src[si] == '%' && src[si + 1] && src[si + 2])
        {
            char h[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(h, NULL, 16);
            si += 2;
        }
        else if (src[si] == '+')
        {
            dst[di++] = ' ';
        }
        else
        {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/* Pull a key=value pair from an x-www-form-urlencoded body */
static bool form_get(const char *body, const char *key,
                     char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p)
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            char tmp[256];
            if (vlen >= sizeof(tmp))
                vlen = sizeof(tmp) - 1;
            memcpy(tmp, v, vlen);
            tmp[vlen] = '\0';
            url_decode(out, tmp, out_len);
            return true;
        }
        p = strchr(p, '&');
        if (!p)
            break;
        p++;
    }
    return false;
}

/* ── RSSI → number of lit bars (1–4) ─────────────────────────── */
static int rssi_bars(int8_t rssi)
{
    if (rssi >= -50)
        return 4;
    if (rssi >= -60)
        return 3;
    if (rssi >= -70)
        return 2;
    return 1;
}

/* ── HTTP handlers ────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);

    /* Build network rows */
    char row[512];
    for (uint16_t i = 0; i < s_scan_count; i++)
    {
        wifi_ap_record_t *ap = &s_scan_list[i];
        int bars = rssi_bars(ap->rssi);
        bool secured = (ap->authmode != WIFI_AUTH_OPEN);

        /* Sanitise SSID for HTML (simple quote escaping) */
        char safe_ssid[65] = {0};
        for (int si = 0, di = 0;
             ap->ssid[si] && di < (int)sizeof(safe_ssid) - 1; si++)
        {
            char c = (char)ap->ssid[si];
            if (c == '\'' || c == '"' || c == '<' || c == '>' || c == '&')
            {
                /* skip unsafe chars */
            }
            else
            {
                safe_ssid[di++] = c;
            }
        }

        snprintf(row, sizeof(row),
                 "<div class='net' data-ssid='%s'>"
                 "<div class='bars'>"
                 "<div class='bar%s' style='height:5px'></div>"
                 "<div class='bar%s' style='height:9px'></div>"
                 "<div class='bar%s' style='height:13px'></div>"
                 "<div class='bar%s' style='height:18px'></div>"
                 "</div>"
                 "<span class='ssid'>%s</span>"
                 "%s"
                 "</div>",
                 safe_ssid,
                 bars >= 1 ? " lit" : "",
                 bars >= 2 ? " lit" : "",
                 bars >= 3 ? " lit" : "",
                 bars >= 4 ? " lit" : "",
                 safe_ssid,
                 secured ? "<span class='lock'>&#128274;</span>" : "");

        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }

    if (s_scan_count == 0)
    {
        httpd_resp_send_chunk(req,
                              "<p style='color:#666680;text-align:center'>No networks found.</p>",
                              HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_connect(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 256)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Bad request");
        return ESP_OK;
    }

    char body[257] = {0};
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Read error");
        return ESP_OK;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};

    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0')
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing SSID");
        return ESP_OK;
    }
    form_get(body, "password", pass, sizeof(pass));

    ESP_LOGI(TAG, "Portal: connect request for SSID '%s'", ssid);

    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    strncpy(s_result_ssid, ssid, sizeof(s_result_ssid) - 1);
    strncpy(s_result_pass, pass, sizeof(s_result_pass) - 1);
    s_has_result = true;
    xSemaphoreGive(s_result_mutex);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Connecting... you can close this tab.");
    return ESP_OK;
}

/* Redirect everything else to the root page (captive portal behaviour) */
static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Scan task ────────────────────────────────────────────────── */

static void scan_task(void *arg)
{
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    /* Blocking scan — runs in its own task so it doesn't block httpd */
    esp_wifi_scan_start(&sc, true);

    uint16_t n = SCAN_MAX;
    esp_wifi_scan_get_ap_records(&n, s_scan_list);
    s_scan_count = n;
    ESP_LOGI(TAG, "Portal: scan found %u networks", n);

    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────── */

void wifi_portal_start(void)
{
    if (s_running)
        return;

    if (!s_result_mutex)
        s_result_mutex = xSemaphoreCreateMutex();

    s_has_result = false;
    s_scan_count = 0;
    s_result_ssid[0] = '\0';
    s_result_pass[0] = '\0';

    /* Switch to APSTA so we can scan while serving the AP */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure the soft-AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = sizeof(AP_SSID) - 1,
            .channel = AP_CHAN,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = AP_MAX_STA,
        },
    };

    if (s_ap_netif == NULL)
        s_ap_netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start HTTP server */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 8;
    hcfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &hcfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = handle_root};
    httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = handle_connect};
    httpd_uri_t redir_uri = {.uri = "/*", .method = HTTP_GET, .handler = handle_redirect};

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &connect_uri);
    httpd_register_uri_handler(s_server, &redir_uri);

    /* Trigger a network scan in the background */
    xTaskCreate(scan_task, "portal_scan", 4096, NULL, 5, NULL);

    s_running = true;
    ESP_LOGI(TAG, "Portal started — SSID: %s, IP: 192.168.4.1", AP_SSID);
}

void wifi_portal_stop(void)
{
    if (!s_running)
        return;

    if (s_server)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);

    s_has_result = false;
    s_running = false;
    ESP_LOGI(TAG, "Portal stopped");
}

bool wifi_portal_has_result(void)
{
    if (!s_result_mutex)
        return false;
    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    bool r = s_has_result;
    xSemaphoreGive(s_result_mutex);
    return r;
}

void wifi_portal_get_result(char *ssid_out, size_t ssid_len,
                            char *pass_out, size_t pass_len)
{
    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    strncpy(ssid_out, s_result_ssid, ssid_len - 1);
    ssid_out[ssid_len - 1] = '\0';
    strncpy(pass_out, s_result_pass, pass_len - 1);
    pass_out[pass_len - 1] = '\0';
    s_has_result = false;
    xSemaphoreGive(s_result_mutex);
}
