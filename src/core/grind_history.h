#pragma once

/*
 * grind_history — circular buffer of completed grind shots, persisted in NVS.
 *
 * NVS namespace: "grind_hist"
 * Keys: "count" (u16), "head" (u16), "records" (blob)
 *
 * Call grind_history_init() once at startup (after nvs_flash_init).
 * Call grind_history_record() every time a grind cycle completes.
 */

#define HISTORY_MAX 50

typedef struct {
    float target_g;
    float result_g;
} grind_record_t;

/* Load persisted history from NVS (safe to call if NVS is empty). */
void grind_history_init(void);

/* Append a completed shot. Overwrites oldest entry when full. */
void grind_history_record(float target_g, float result_g);

/* Number of records currently stored (0–HISTORY_MAX). */
int  grind_history_count(void);

/*
 * Copy up to `max` records into `out`, ordered oldest → newest.
 * Returns the number of entries written.
 */
int  grind_history_get(grind_record_t *out, int max);

/* Erase all history from RAM and NVS. */
void grind_history_clear(void);
