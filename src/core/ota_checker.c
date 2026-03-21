/*
 * ota_checker — background GitHub release check + cloud OTA
 *
 * Flow:
 *   1. check_task: waits for WiFi, hits the GitHub Releases API, parses
 *      tag_name and the .bin browser_download_url, sets state to
 *      AVAILABLE or NO_UPDATE.
 *   2. dl_task (started by ota_checker_apply): streams the binary into
 *      the next OTA partition via esp_https_ota, then reboots.
 */

#include "ota_checker.h"
#include "../version.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ota_checker";

#define API_URL        "https://api.github.com/repos/josomm22/rocky-3000/releases/latest"
#define API_BUF_SIZE   16384   /* 16 KB — enough for a typical release JSON */
#define WIFI_WAIT_SECS 60

/* ── Shared state (written by tasks, read by LVGL poll timer) ── */
static volatile ota_check_state_t s_state    = OTA_CHECK_IDLE;
static volatile int               s_progress = 0;
static char s_new_version[24] = {0};
static char s_bin_url[256]    = {0};

/* ── Version comparison ───────────────────────────────────────── */

static void parse_ver(const char *s, int *maj, int *min, int *pat)
{
    *maj = *min = *pat = 0;
    const char *p = (s[0] == 'v') ? s + 1 : s;
    sscanf(p, "%d.%d.%d", maj, min, pat);
}

/* Returns true when remote tag is strictly newer than the running build. */
static bool version_newer(const char *remote, const char *current)
{
    /* current may have a git-describe suffix: "1.0.1-3-gabcdef" */
    char cur_clean[24] = {0};
    const char *c = (current[0] == 'v') ? current + 1 : current;
    const char *dash = strchr(c, '-');
    if (dash) {
        size_t n = (size_t)(dash - c);
        if (n >= sizeof(cur_clean)) n = sizeof(cur_clean) - 1;
        memcpy(cur_clean, c, n);
    } else {
        strncpy(cur_clean, c, sizeof(cur_clean) - 1);
    }

    int rmaj, rmin, rpat, cmaj, cmin, cpat;
    parse_ver(remote, &rmaj, &rmin, &rpat);
    parse_ver(cur_clean, &cmaj, &cmin, &cpat);

    if (rmaj != cmaj) return rmaj > cmaj;
    if (rmin != cmin) return rmin > cmin;
    return rpat > cpat;
}

/* ── Minimal JSON field extraction ───────────────────────────── */

static bool json_str_field(const char *json, const char *key,
                            char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\": \"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* Scans for the first browser_download_url whose value ends in ".bin". */
static bool find_bin_url(const char *json, char *out, size_t out_len)
{
    const char *KEY = "\"browser_download_url\": \"";
    const char *q = json;
    while ((q = strstr(q, KEY)) != NULL) {
        q += strlen(KEY);
        const char *end = strchr(q, '"');
        if (!end) break;
        size_t len = (size_t)(end - q);
        if (len > 4 && strncmp(end - 4, ".bin", 4) == 0) {
            if (len < out_len) {
                memcpy(out, q, len);
                out[len] = '\0';
                return true;
            }
        }
        q = end + 1;
    }
    return false;
}

/* ── Check task ───────────────────────────────────────────────── */

static void check_task(void *arg)
{
    (void)arg;

    /* Wait up to WIFI_WAIT_SECS for a DHCP lease */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool wifi_ok = false;
    for (int i = 0; i < WIFI_WAIT_SECS; i++) {
        esp_netif_ip_info_t info = {0};
        if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
            wifi_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!wifi_ok) {
        ESP_LOGW(TAG, "No WiFi after %d s — will retry when connected", WIFI_WAIT_SECS);
        s_state = OTA_CHECK_IDLE;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Checking for firmware updates...");

    char *buf = malloc(API_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for API buffer");
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url               = API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .user_agent        = "GBWUI/" APP_VERSION_STRING,
        .method            = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(buf);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "GitHub API returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        s_state = OTA_CHECK_NO_UPDATE;
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    while (total < API_BUF_SIZE - 1) {
        int n = esp_http_client_read(client, buf + total, API_BUF_SIZE - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Parse tag_name */
    char tag[32] = {0};
    if (!json_str_field(buf, "tag_name", tag, sizeof(tag))) {
        ESP_LOGW(TAG, "tag_name not found in response");
        free(buf);
        s_state = OTA_CHECK_NO_UPDATE;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Latest: %s  Running: %s", tag, APP_VERSION_STRING);

    if (!version_newer(tag, APP_VERSION_STRING)) {
        ESP_LOGI(TAG, "Firmware is up to date");
        free(buf);
        s_state = OTA_CHECK_NO_UPDATE;
        vTaskDelete(NULL);
        return;
    }

    /* Find the .bin asset URL — optional; we still notify even without one */
    char bin_url[256] = {0};
    if (find_bin_url(buf, bin_url, sizeof(bin_url))) {
        strncpy(s_bin_url, bin_url, sizeof(s_bin_url) - 1);
        ESP_LOGI(TAG, "Update available: %s  URL: %s", tag, bin_url);
    } else {
        s_bin_url[0] = '\0';
        ESP_LOGW(TAG, "Update available: %s  (no .bin asset — OTA install unavailable)", tag);
    }

    free(buf);

    strncpy(s_new_version, tag, sizeof(s_new_version) - 1);
    s_state = OTA_CHECK_AVAILABLE;
    vTaskDelete(NULL);
}

/* ── Download + flash task ────────────────────────────────────── */

static void dl_task(void *arg)
{
    (void)arg;
    s_state    = OTA_CHECK_DOWNLOADING;
    s_progress = 0;

    ESP_LOGI(TAG, "Starting OTA download from %s", s_bin_url);

    esp_http_client_config_t http_cfg = {
        .url                  = s_bin_url,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .timeout_ms           = 60000,
        .keep_alive_enable    = true,
        .buffer_size          = 4096,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int total = esp_https_ota_get_image_size(ota_handle);
        int read  = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            s_progress = read * 100 / total;
        }
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA: incomplete data received");
        esp_https_ota_abort(ota_handle);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t finish_err = esp_https_ota_finish(ota_handle);
    if (finish_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(finish_err));
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA complete — rebooting in 2 s");
    s_progress = 100;
    s_state    = OTA_CHECK_DONE;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────── */

void ota_checker_start(void)
{
    if (s_state != OTA_CHECK_IDLE) return;
    s_state = OTA_CHECK_CHECKING;
    xTaskCreate(check_task, "ota_check", 16384, NULL, 3, NULL);
}

void ota_checker_recheck(void)
{
    /* Allow a fresh check from any terminal state; no-op if already running */
    if (s_state == OTA_CHECK_CHECKING  ||
        s_state == OTA_CHECK_DOWNLOADING ||
        s_state == OTA_CHECK_DONE)
        return;
    s_state = OTA_CHECK_IDLE;
    ota_checker_start();
}

ota_check_state_t ota_checker_get_state(void)   { return s_state; }
const char       *ota_checker_get_version(void)  { return s_new_version; }
int               ota_checker_get_progress(void) { return s_progress; }

bool ota_checker_has_binary(void)
{
    return s_bin_url[0] != '\0';
}

void ota_checker_apply(void)
{
    if (s_state != OTA_CHECK_AVAILABLE) return;
    if (s_bin_url[0] == '\0') return;   /* no asset attached to this release */
    xTaskCreate(dl_task, "ota_dl", 8192, NULL, 5, NULL);
}
