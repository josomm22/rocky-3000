/*
 * ota_checker — background GitHub release check + cloud OTA
 *
 * Flow:
 *   1. check_task: waits for WiFi, syncs NTP, then does a GET to
 *      https://github.com/{repo}/releases/latest with redirects disabled.
 *      GitHub responds with 302 and a Location header pointing to
 *      .../releases/tag/vX.Y.Z — the tag is parsed from that URL and
 *      compared with the running firmware.  No JSON, no API token needed.
 *   2. dl_task (started by ota_checker_apply): streams the binary into
 *      the next OTA partition via esp_http_client_perform() and
 *      esp_ota_write() in the HTTP_EVENT_ON_DATA callback, then reboots.
 *      See the long comment above dl_event_cb for why perform() is used
 *      instead of esp_https_ota_*.
 *   A periodic_task re-checks every 4 hours.
 */

#include "ota_checker.h"
#include "../version.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/dns.h"
#include "esp_log.h"
#include "esp_system.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "ota_checker";

#define RELEASES_URL       "https://github.com/josomm22/rocky-3000/releases/latest"
#define BIN_URL_FMT        "https://github.com/josomm22/rocky-3000/releases/download/%s/gbwui-%s.bin"
#define WIFI_WAIT_SECS     60
#define RECHECK_INTERVAL_S (4 * 60 * 60)   /* re-check every 4 hours */

/* ── Shared state (written by tasks, read by LVGL poll timer) ── */
static volatile ota_check_state_t s_state    = OTA_CHECK_IDLE;
static volatile int               s_progress = 0;
static char s_new_version[24] = {0};
static char s_bin_url[256]    = {0};
static bool     s_periodic_started = false;
static int      s_http_status      = 0;    /* last HTTP status code, 0 = no response */
static esp_err_t s_open_err        = ESP_OK; /* esp_http_client_open error, when status=0 */
static int      s_tls_err          = 0;    /* mbedTLS error code, 0 = no TLS error */

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

/* ── Check task ───────────────────────────────────────────────── */

/* Event handler that captures the Location header and any TLS error */
static esp_err_t check_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(evt->header_key, "location") == 0) {
        char *dst = (char *)evt->user_data;
        strncpy(dst, evt->header_value, 255);
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        int tls_code = 0, tls_flags = 0;
        esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                         &tls_code, &tls_flags);
        if (tls_code) {
            s_tls_err = tls_code;
            ESP_LOGE(TAG, "TLS error: -0x%04x  flags: 0x%04x", -tls_code, tls_flags);
        }
    }
    return ESP_OK;
}

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

    /* Brief settle: IP address obtained doesn't mean routing is ready yet */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Free heap before HTTPS: %lu bytes", esp_get_free_heap_size());

    /* Override DNS with Google's servers — DHCP-provided DNS may return
     * NXDOMAIN for external hosts on restricted/split-horizon networks */
    ip_addr_t dns0, dns1;
    IP_ADDR4(&dns0, 8, 8, 8, 8);
    IP_ADDR4(&dns1, 8, 8, 4, 4);
    dns_setserver(0, &dns0);
    dns_setserver(1, &dns1);

    /* Sync system clock — mbedTLS rejects certs when time is at epoch 0 */
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }
    for (int i = 0; i < 15; i++) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
        ESP_LOGW(TAG, "NTP sync timed out — TLS cert check may fail");

    s_http_status = 0;
    s_open_err    = ESP_OK;
    s_tls_err     = 0;
    ESP_LOGI(TAG, "Checking for firmware updates via redirect check...");

    /* GET github.com/releases/latest with redirects disabled.
     * GitHub returns 301 → .../releases/tag/vX.Y.Z — parse tag from URL.
     * This avoids the JSON API entirely: no auth token, no large buffer. */
    char location[256] = {0};

    esp_http_client_config_t cfg = {
        .url                   = RELEASES_URL,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 20000,
        .user_agent            = "GBWUI/" APP_VERSION_STRING,
        .disable_auto_redirect = true,
        .max_redirection_count = 0,
        .buffer_size           = 2048,
        .buffer_size_tx        = 2048,
        .event_handler         = check_event_cb,
        .user_data             = location,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s (0x%x)", esp_err_to_name(err), err);
        s_open_err = err;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    s_http_status = status;
    ESP_LOGI(TAG, "HTTP %d  Location: %s", status, location);

    if (status != 301 && status != 302) {
        ESP_LOGW(TAG, "Unexpected status %d — expected redirect", status);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    /* Extract tag from Location: .../releases/tag/vX.Y.Z */
    const char *tag_prefix = "/tag/";
    const char *tag_start  = strstr(location, tag_prefix);
    if (!tag_start) {
        ESP_LOGW(TAG, "No /tag/ in Location header: %s", location);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    tag_start += strlen(tag_prefix);

    char tag[32] = {0};
    strncpy(tag, tag_start, sizeof(tag) - 1);
    /* Trim any trailing path/query */
    char *p = tag;
    while (*p && *p != '/' && *p != '?') p++;
    *p = '\0';

    ESP_LOGI(TAG, "Latest: %s  Running: %s", tag, APP_VERSION_STRING);

    if (!version_newer(tag, APP_VERSION_STRING)) {
        ESP_LOGI(TAG, "Firmware is up to date");
        s_state = OTA_CHECK_NO_UPDATE;
        vTaskDelete(NULL);
        return;
    }

    /* Construct download URL from known naming convention (see release.yml) */
    snprintf(s_bin_url, sizeof(s_bin_url), BIN_URL_FMT, tag, tag);
    strncpy(s_new_version, tag, sizeof(s_new_version) - 1);
    ESP_LOGI(TAG, "Update available: %s  URL: %s", tag, s_bin_url);
    s_state = OTA_CHECK_AVAILABLE;
    vTaskDelete(NULL);
}

/* ── Download + flash task ────────────────────────────────────── */

/*
 * Streams the binary using esp_http_client_perform().
 *
 * Why perform() and not esp_https_ota_begin():
 *   - esp_https_ota_* uses the open/fetch_headers/read streaming path which
 *     historically cannot re-establish TLS across hosts on a 302 redirect.
 *     GitHub returns releases/download/... → objects.githubusercontent.com
 *     which is a different host.
 *   - perform() does handle that cross-host HTTPS→HTTPS redirect correctly
 *     by closing the first TLS session and opening a new one.
 *
 * Why buffer_size_tx = 4096:
 *   - The redirected URL is a presigned S3-style URL with a long query
 *     string (X-Amz-Signature, X-Amz-Credential, etc.), commonly
 *     1.5–2 KB. The default TX buffer (512 B) cannot hold the full
 *     "GET <path> HTTP/1.1\r\n" request line, which causes the request
 *     to be truncated and the connection to be torn down almost
 *     immediately with no useful error — matching the failure mode
 *     observed in practice.
 *
 * Why keep_alive_enable is NOT set:
 *   - Cross-host redirect must close the connection anyway.  Leaving
 *     keep-alive on caused spurious "connection reset" errors.
 *
 * Firmware chunks arrive via HTTP_EVENT_ON_DATA and are written directly
 * into the next OTA partition — same pattern as the working web-upload
 * endpoint in web_server.c.  Image validation is performed by
 * esp_ota_end(), which checks the SHA-256 stored in the image trailer.
 */

typedef struct {
    esp_ota_handle_t handle;
    int              content_len;
    int              total_read;
    esp_err_t        write_err;
    bool             header_checked;
} dl_ctx_t;

static esp_err_t dl_event_cb(esp_http_client_event_t *evt)
{
    dl_ctx_t *ctx = (dl_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR: {
        int tls_code = 0, tls_flags = 0;
        esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                         &tls_code, &tls_flags);
        if (tls_code) {
            s_tls_err = tls_code;
            ESP_LOGE(TAG, "OTA TLS error: -0x%04x  flags: 0x%04x",
                     -tls_code, tls_flags);
        }
        break;
    }
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "content-length") == 0) {
            ctx->content_len = atoi(evt->header_value);
            ESP_LOGI(TAG, "OTA: content-length = %d", ctx->content_len);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len <= 0 || ctx->write_err != ESP_OK) break;

        /* Sanity-check the first byte: ESP32 app images start with the
         * magic byte 0xE9 (esp_image_header_t::magic).  Bailing out here
         * prevents us from flashing an HTML error page if GitHub ever
         * returns a 404 body past the redirect chain. */
        if (!ctx->header_checked) {
            ctx->header_checked = true;
            uint8_t first = ((uint8_t *)evt->data)[0];
            if (first != 0xE9) {
                ESP_LOGE(TAG,
                         "OTA: first byte 0x%02x != ESP image magic 0xE9"
                         " — server returned non-image body", first);
                ctx->write_err = ESP_ERR_OTA_VALIDATE_FAILED;
                break;
            }
        }

        ctx->write_err = esp_ota_write(ctx->handle, evt->data, evt->data_len);
        ctx->total_read += evt->data_len;
        if (ctx->content_len > 0)
            s_progress = ctx->total_read * 100 / ctx->content_len;
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void dl_task(void *arg)
{
    (void)arg;
    s_state       = OTA_CHECK_DOWNLOADING;
    s_progress    = 0;
    s_http_status = 0;
    s_open_err    = ESP_OK;
    s_tls_err     = 0;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "OTA: no update partition");
        s_open_err = ESP_ERR_NOT_FOUND;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA: writing to partition '%s' @ 0x%08" PRIx32,
             part->label, part->address);

    dl_ctx_t ctx = { .write_err = ESP_OK };
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ctx.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        s_open_err = err;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA: downloading %s", s_bin_url);
    ESP_LOGI(TAG, "OTA: free heap before GET: %lu bytes", esp_get_free_heap_size());

    esp_http_client_config_t cfg = {
        .url                   = s_bin_url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 60000,
        .buffer_size           = 4096,  /* RX */
        .buffer_size_tx        = 4096,  /* TX — must fit long presigned URL */
        .max_redirection_count = 5,
        .user_agent            = "GBWUI/" APP_VERSION_STRING,
        .event_handler         = dl_event_cb,
        .user_data             = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "OTA: client init failed");
        esp_ota_abort(ctx.handle);
        s_open_err = ESP_ERR_NO_MEM;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t content_len = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "OTA: perform done — err=%s  status=%d  got=%d/%lld",
             esp_err_to_name(err), status, ctx.total_read, (long long)content_len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: perform failed: %s", esp_err_to_name(err));
        s_open_err = err;
        esp_ota_abort(ctx.handle);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "OTA: unexpected HTTP %d", status);
        s_http_status = status;
        esp_ota_abort(ctx.handle);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    if (ctx.write_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: flash write/validate failed: %s",
                 esp_err_to_name(ctx.write_err));
        s_open_err = ctx.write_err;
        esp_ota_abort(ctx.handle);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    if (ctx.total_read == 0) {
        ESP_LOGE(TAG, "OTA: zero bytes received");
        s_open_err = ESP_ERR_INVALID_SIZE;
        esp_ota_abort(ctx.handle);
        s_state = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(ctx.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        s_open_err = err;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        s_open_err = err;
        s_state    = OTA_CHECK_ERROR;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes) — rebooting in 2 s", ctx.total_read);
    s_progress = 100;
    s_state    = OTA_CHECK_DONE;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    vTaskDelete(NULL);
}

/* ── Periodic re-check task ───────────────────────────────────── */

static void periodic_task(void *arg)
{
    (void)arg;
    /* Count up in 1-minute increments to avoid pdMS_TO_TICKS overflow */
    int elapsed_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));   /* sleep 1 minute */
        elapsed_s += 60;
        if (elapsed_s >= RECHECK_INTERVAL_S) {
            elapsed_s = 0;
            ESP_LOGI(TAG, "Periodic re-check triggered");
            ota_checker_recheck();
        }
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void ota_checker_start(void)
{
    if (s_state != OTA_CHECK_IDLE) return;
    s_state = OTA_CHECK_CHECKING;
    xTaskCreate(check_task, "ota_check", 24576, NULL, 3, NULL);
    if (!s_periodic_started) {
        s_periodic_started = true;
        xTaskCreate(periodic_task, "ota_periodic", 2048, NULL, 1, NULL);
    }
}

void ota_checker_recheck(void)
{
    /* No-op if already running, if an update is waiting for the user,
     * or if a download/install is in progress */
    if (s_state == OTA_CHECK_CHECKING    ||
        s_state == OTA_CHECK_AVAILABLE   ||
        s_state == OTA_CHECK_DOWNLOADING ||
        s_state == OTA_CHECK_DONE)
        return;
    s_state = OTA_CHECK_IDLE;
    ota_checker_start();
}

void ota_checker_force_check(void)
{
    /* Only blocks if already in flight or done */
    if (s_state == OTA_CHECK_CHECKING    ||
        s_state == OTA_CHECK_DOWNLOADING ||
        s_state == OTA_CHECK_DONE)
        return;
    s_state = OTA_CHECK_IDLE;
    ota_checker_start();
}

ota_check_state_t ota_checker_get_state(void)      { return s_state; }
const char       *ota_checker_get_version(void)    { return s_new_version; }
int               ota_checker_get_progress(void)   { return s_progress; }
int               ota_checker_get_http_status(void){ return s_http_status; }
int               ota_checker_get_open_err(void)   { return (int)s_open_err; }
int               ota_checker_get_tls_err(void)    { return s_tls_err; }

bool ota_checker_has_binary(void)
{
    return s_bin_url[0] != '\0';
}

void ota_checker_apply(void)
{
    if (s_state != OTA_CHECK_AVAILABLE) return;
    if (s_bin_url[0] == '\0') return;   /* no asset attached to this release */
    xTaskCreate(dl_task, "ota_dl", 16384, NULL, 5, NULL);
}
