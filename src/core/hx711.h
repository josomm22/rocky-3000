#pragma once

/*
 * Minimal bit-bang HX711 driver for GBWUI.
 *
 * Pins are configured at hx711_init() time.  All functions are intended
 * to be called from a single task (hx711_task on Core 0).
 *
 * Protocol summary:
 *   - DOUT low → conversion ready
 *   - 24 CLK pulses read the result MSB-first (two's complement)
 *   - 1 extra CLK pulse after read → Channel A, Gain 128 for next sample
 */

#include <stdbool.h>
#include "driver/gpio.h"

/*
 * Initialise GPIO directions, pull CLK low.
 * Call once before any other hx711_* function.
 */
void hx711_init(gpio_num_t dout, gpio_num_t pd_sck);

/*
 * Block until TARE_SAMPLES readings are available, average them, and store
 * as the zero offset.  Intended to be called once at startup from hx711_task.
 */
void hx711_tare(void);

/* True when DOUT is low (conversion complete). */
bool hx711_is_ready(void);

/*
 * If a sample is ready, read it, apply cal_factor, subtract tare offset,
 * store the result in *out_g, and return true.
 * Returns false (and leaves *out_g unchanged) when not ready.
 */
bool hx711_read_grams(float cal_factor, float *out_g);
