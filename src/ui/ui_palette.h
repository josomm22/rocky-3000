#pragma once

#include "lvgl.h"

/* ── Colour palette ─────────────────────────────────────────── */
#define COL_BG lv_color_hex(0x111118)        /* near-black background       */
#define COL_SURFACE lv_color_hex(0x1e1e2e)   /* slightly raised surface     */
#define COL_PRESET_BG lv_color_hex(0x25253a) /* unselected pill fill        */
#define COL_ACCENT lv_color_hex(0x4fc3f7)    /* selection highlight         */
#define COL_TEXT lv_color_hex(0xe8e8f0)      /* primary text                */
#define COL_TEXT_DIM lv_color_hex(0x666680)  /* secondary / muted text      */
#define COL_GRIND_BG lv_color_hex(0x1b5e20)  /* GRIND button base           */
#define COL_GRIND_HL lv_color_hex(0x4caf50)  /* GRIND button border/pressed */
#define COL_GEAR lv_color_hex(0x909090)      /* settings icon               */
#define COL_ERROR lv_color_hex(0xb71c1c)     /* error / delete red          */
#define COL_SUCCESS lv_color_hex(0x1b5e20)   /* success / toast green       */

/* ── Layout constants ───────────────────────────────────────── */
#define PRESET_MAX 6
#define PILL_W 106
#define PILL_H 57
#define PILL_RADIUS (PILL_H / 2)
#define PILL_GAP 14
#define SEL_FRAME_W (PILL_W + 10)
#define SEL_FRAME_H (PILL_H + 10)

/* Screen logical size (LVGL display after 270° rotation) */
#define SCR_W 640
#define SCR_H 480
