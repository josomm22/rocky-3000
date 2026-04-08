/*
 * grind_history — circular buffer of completed grind shots, persisted in NVS.
 */

#include "grind_history.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdbool.h>

#define NVS_NS   "grind_hist"
#define KEY_VER  "ver"
#define KEY_CNT  "count"
#define KEY_HEAD "head"
#define KEY_DATA "records"

static grind_record_t s_buf[HISTORY_MAX];
static int s_count = 0;   /* valid entries, 0–HISTORY_MAX */
static int s_head  = 0;   /* next write slot              */

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h,  KEY_VER,  (uint8_t)HISTORY_VER);
    nvs_set_u16(h, KEY_CNT,  (uint16_t)s_count);
    nvs_set_u16(h, KEY_HEAD, (uint16_t)s_head);
    nvs_set_blob(h, KEY_DATA, s_buf, sizeof(s_buf));
    nvs_commit(h);
    nvs_close(h);
}

void grind_history_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    /* Version check — wipe incompatible records rather than loading garbage */
    uint8_t ver = 0;
    nvs_get_u8(h, KEY_VER, &ver);
    if (ver != HISTORY_VER) {
        nvs_erase_key(h, KEY_DATA);
        nvs_erase_key(h, KEY_CNT);
        nvs_erase_key(h, KEY_HEAD);
        nvs_set_u8(h, KEY_VER, (uint8_t)HISTORY_VER);
        nvs_commit(h);
        nvs_close(h);
        return; /* starts fresh */
    }

    uint16_t cnt = 0, head = 0;
    nvs_get_u16(h, KEY_CNT,  &cnt);
    nvs_get_u16(h, KEY_HEAD, &head);
    size_t sz = sizeof(s_buf);
    nvs_get_blob(h, KEY_DATA, s_buf, &sz);
    nvs_close(h);
    if (cnt  <= HISTORY_MAX) s_count = cnt;
    if (head <  HISTORY_MAX) s_head  = head;
}

void grind_history_record(float target_g, float result_g,
                          float weight_at_cutoff_g, float weight_before_pulses_g,
                          float flow_g_s, float offset_g,
                          uint32_t timestamp, uint16_t grind_ms,
                          uint8_t pulse_count)
{
    s_buf[s_head].target_g              = target_g;
    s_buf[s_head].result_g              = result_g;
    s_buf[s_head].weight_at_cutoff_g    = weight_at_cutoff_g;
    s_buf[s_head].weight_before_pulses_g = weight_before_pulses_g;
    s_buf[s_head].flow_g_s              = flow_g_s;
    s_buf[s_head].offset_g              = offset_g;
    s_buf[s_head].timestamp             = timestamp;
    s_buf[s_head].grind_ms              = grind_ms;
    s_buf[s_head].pulse_count           = pulse_count;
    s_buf[s_head]._pad                  = 0;
    s_head = (s_head + 1) % HISTORY_MAX;
    if (s_count < HISTORY_MAX) s_count++;
    nvs_save();
}

int grind_history_count(void) { return s_count; }

int grind_history_get(grind_record_t *out, int max)
{
    int n = s_count < max ? s_count : max;
    int start = (s_head - s_count + HISTORY_MAX * 2) % HISTORY_MAX;
    for (int i = 0; i < n; i++)
        out[i] = s_buf[(start + i) % HISTORY_MAX];
    return n;
}

void grind_history_clear(void)
{
    s_count = 0;
    s_head  = 0;
    memset(s_buf, 0, sizeof(s_buf));
    nvs_save();
}

void grind_history_delete_indices(const int *del, int del_count)
{
    grind_record_t tmp[HISTORY_MAX];
    int start     = (s_head - s_count + HISTORY_MAX * 2) % HISTORY_MAX;
    int new_count = 0;

    for (int i = 0; i < s_count; i++) {
        bool skip = false;
        for (int j = 0; j < del_count; j++) {
            if (del[j] == i) { skip = true; break; }
        }
        if (!skip)
            tmp[new_count++] = s_buf[(start + i) % HISTORY_MAX];
    }

    memset(s_buf, 0, sizeof(s_buf));
    memcpy(s_buf, tmp, new_count * sizeof(grind_record_t));
    s_count = new_count;
    s_head  = new_count % HISTORY_MAX;
    nvs_save();
}
