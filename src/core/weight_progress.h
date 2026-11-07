#ifndef WEIGHT_PROGRESS_H
#define WEIGHT_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

#define WEIGHT_PROGRESS_TIMEOUT_MS 3000u
#define WEIGHT_PROGRESS_MIN_G 0.2f

typedef struct {
    float weight;
    uint32_t last_progress_ms;
} weight_progress_t;

/* Only increases beyond the previous high count as progress. Negative
 * readings and vibration around zero cannot keep a failed shot running. */
static inline bool weight_progress_expired(weight_progress_t *progress,
                                           float weight, uint32_t now)
{
    if (weight >= progress->weight + WEIGHT_PROGRESS_MIN_G) {
        progress->weight = weight;
        progress->last_progress_ms = now;
    }
    return (uint32_t)(now - progress->last_progress_ms) >= WEIGHT_PROGRESS_TIMEOUT_MS;
}

#endif
