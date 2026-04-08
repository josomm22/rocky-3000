#pragma once
/* Minimal ESP-IDF nvs.h stub for host-native unit tests */

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);

esp_err_t nvs_commit(nvs_handle_t handle);
