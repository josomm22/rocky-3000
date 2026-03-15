# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**GBWUI (Grind By Weight UI)** — an ESP32-S3 embedded application for a coffee grinder controller. It monitors live weight via an HX711 load cell, controls the grinder via an SSR on GPIO43, and presents a touch UI on a 2.8" capacitive touch LCD (480×640, ST7701S, GT911 touch). UI framework is LVGL 9.x on ESP-IDF.

## Build Commands

```bash
# Build firmware
pio run -e waveshare_28b

# Build + flash
pio run -e waveshare_28b -t upload

# Serial monitor (115200 baud)
pio device monitor -b 115200

# Clean build
pio run -e waveshare_28b -t clean
```

There are no unit tests. Validation is done via serial monitor and on-device observation.

## Architecture

### Entry Point
`src/main.c` — initializes all hardware, loads NVS settings, auto-connects WiFi, starts the grind controller and shot history, launches the HTTP server, then runs the LVGL main loop on Core 1.

### Layer Structure

```
Hardware Drivers         →  LCD_Driver/, Touch_Driver/, I2C_Driver/, EXIO/, Buzzer/
Core Logic               →  core/grind_controller.c, core/grind_history.c
UI Screens               →  ui/screen_*.c
UI Infrastructure        →  ui/display_manager.c, ui/ui_palette.h
Network                  →  ui/wifi_portal.c, ui/web_server.c
```

### Key Files

| File | Purpose |
|------|---------|
| `src/core/grind_controller.c` | Grind state machine, SSR control, HX711 polling, auto-tune offset |
| `src/ui/screen_main.c` | Main screen: preset pills, GRIND button, live weight, toasts |
| `src/ui/screen_settings.c` | Settings: brightness slider, sleep timeout, WiFi/Calibration/OTA nav |
| `src/ui/screen_calibration.c` | 3-step calibration wizard |
| `src/ui/screen_wifi.c` | WiFi scan/connect with LVGL keyboard |
| `src/ui/screen_ota.c` | OTA update UI with progress bar |
| `src/ui/web_server.c` | HTTP server: `/ota`, `/update` (POST), `/history`, `/api/history` |
| `src/ui/wifi_portal.c` | Background WiFi auto-connect + NTP sync to PCF85063 RTC |
| `src/ui/display_manager.c` | Brightness and sleep timeout management |
| `src/ui/ui_palette.h` | Color constants and layout values |

### Grind Controller State Machine

```
GRIND_IDLE → GRIND_RUNNING → GRIND_DONE → GRIND_IDLE
```

- `GRIND_DEMO_MODE=0` (active): real HX711 task on Core 0 at 80 Hz, atomic `volatile float` shared with LVGL poll timer
- `GRIND_DEMO_MODE=1`: simulates weight at 3 g/sec with noise (no hardware needed)
- Auto-tune: post-grind `new_offset = clamp(offset + delta × 0.5, 0.0, 5.0)`, converges in ~3 shots

### FreeRTOS Tasks

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| LVGL main loop | 1 | 2 | UI tick + display flush |
| `hx711_task` | 0 | 5 | HX711 @ 80 Hz (real mode only) |
| `ota_srv_task` | 0 | 2 | HTTP server (persistent) |

### NVS Persistence

Stored across three namespaces:
- **default**: `preset_count`, `preset_N` (floats), `offset_g`, `cal_factor`, `wifi_ssid`, `wifi_pass`
- **`disp_cfg`**: `brightness` (10–100, default 80), `timeout_min` (0=never, default 10)
- **`grind_hist`**: `count`, `head`, `records` blob — up to 50 `{target_g, result_g}` records

### Flash Partitions

Dual OTA setup: `ota_0` + `ota_1` (3 MB each), 16 KB NVS, 8 KB OTA data, 528 KB FAT storage.

## Hardware Notes

- **I2C bus**: GPIO7 (SCL), GPIO15 (SDA), 400 kHz — serves GT911 touch, TCA9554 GPIO expander
- **TCA9554** (0x20): controls backlight and buzzer via `EXIO/`
- **HX711**: GPIO4 (DATA), GPIO44 (CLK) — real mode active (`GRIND_DEMO_MODE=0`)
- **SSR**: GPIO43, active HIGH — UART0 TXD remapped off this pin at boot via `uart_set_pin`; monitor output via USB Serial/JTAG
- **GPIO33–37**: OPI-PSRAM data lines on ESP32-S3R8 — never use these as GPIO
- Board: Waveshare ESP32-S3-Touch-LCD-2.8B
