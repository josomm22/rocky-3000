/*
 * NVS stub for host-native unit tests.
 *
 * All writes are no-ops.  Reads always return ESP_ERR_NVS_NOT_FOUND,
 * which grind_history_init() handles gracefully (leaves the buffer empty).
 * This lets us test RAM-only behaviour without any file I/O.
 */
#include "nvs.h"

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)name;
    (void)open_mode;
    *out_handle = 1;
    return ESP_OK;
}

void      nvs_close(nvs_handle_t h)  { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
    { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }

esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
    { (void)h; (void)k; (void)v; return ESP_OK; }

esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v)
    { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }

esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
    { (void)h; (void)k; (void)v; return ESP_OK; }

esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
    { (void)h; (void)k; (void)v; (void)len; return ESP_ERR_NVS_NOT_FOUND; }

esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
    { (void)h; (void)k; (void)v; (void)len; return ESP_OK; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
    { (void)h; (void)k; return ESP_OK; }
