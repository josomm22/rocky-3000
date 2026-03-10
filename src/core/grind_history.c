/*
 * grind_history — circular buffer of completed grind shots, persisted in NVS.
 */

#include "grind_history.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define NVS_NS   "grind_hist"
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
    nvs_set_u16(h, KEY_CNT,  (uint16_t)s_count);
    nvs_set_u16(h, KEY_HEAD, (uint16_t)s_head);
    nvs_set_blob(h, KEY_DATA, s_buf, sizeof(s_buf));
    nvs_commit(h);
    nvs_close(h);
}

void grind_history_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t cnt = 0, head = 0;
    nvs_get_u16(h, KEY_CNT,  &cnt);
    nvs_get_u16(h, KEY_HEAD, &head);
    size_t sz = sizeof(s_buf);
    nvs_get_blob(h, KEY_DATA, s_buf, &sz);
    nvs_close(h);
    if (cnt  <= HISTORY_MAX) s_count = cnt;
    if (head <  HISTORY_MAX) s_head  = head;
}

void grind_history_record(float target_g, float result_g)
{
    s_buf[s_head].target_g = target_g;
    s_buf[s_head].result_g = result_g;
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
