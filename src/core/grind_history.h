#pragma once

/*
 * grind_history — circular buffer of completed grind shots, persisted in NVS.
 *
 * NVS namespace: "grind_hist"
 * Keys: "ver" (u8), "count" (u16), "head" (u16), "records" (blob)
 *
 * Call grind_history_init() once at startup (after nvs_flash_init).
 * Call grind_history_record() every time a grind cycle completes.
 */

#include <stdint.h>

#define HISTORY_MAX 50
#define HISTORY_VER 2  /* bump when grind_record_t layout changes */

typedef struct {
    float    target_g;               /* target weight (g)                          */
    float    result_g;               /* final dispensed weight (g)                 */
    float    weight_at_cutoff_g;     /* scale reading when SSR first cut (g)       */
    float    weight_before_pulses_g; /* settled weight before any pulse fires (g)  */
    float    flow_g_s;               /* flow rate at SSR cutoff (g/s)              */
    float    offset_g;               /* auto-tune offset used this shot (g)        */
    uint32_t timestamp;              /* unix epoch seconds (0 if RTC not synced)   */
    uint16_t grind_ms;               /* main SSR-on duration (ms)                  */
    uint8_t  pulse_count;            /* correction pulses fired (0-3)              */
    uint8_t  _pad;                   /* alignment padding                          */
} grind_record_t;                    /* 32 bytes */

/* Load persisted history from NVS (safe to call if NVS is empty). */
void grind_history_init(void);

/* Append a completed shot. Overwrites oldest entry when full. */
void grind_history_record(float target_g, float result_g,
                          float weight_at_cutoff_g, float weight_before_pulses_g,
                          float flow_g_s, float offset_g,
                          uint32_t timestamp, uint16_t grind_ms,
                          uint8_t pulse_count);

/* Number of records currently stored (0–HISTORY_MAX). */
int  grind_history_count(void);

/*
 * Copy up to `max` records into `out`, ordered oldest → newest.
 * Returns the number of entries written.
 */
int  grind_history_get(grind_record_t *out, int max);

/* Erase all history from RAM and NVS. */
void grind_history_clear(void);

/*
 * Delete specific records by 0-based chronological index (as returned by
 * grind_history_get).  Remaining records are compacted and re-saved.
 */
void grind_history_delete_indices(const int *indices, int count);
