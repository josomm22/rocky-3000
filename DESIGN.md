# GBWUI — Grind By Weight UI

## Design Document v0.5

---

## 1. Project Overview

GBWUI is an embedded application running on an ESP32-S3 that provides **grind-by-weight** functionality to any standard coffee grinder. The system monitors live weight via a load cell, controls the grinder through a solid-state relay (SSR), and presents a touch-friendly UI on a 2.8" capacitive touch LCD.

---

## 2. Hardware

| Component             | Details                                                                |
| --------------------- | ---------------------------------------------------------------------- |
| Board                 | Waveshare ESP32-S3-Touch-LCD-2.8B                                      |
| MCU                   | ESP32-S3                                                               |
| Display               | 2.8" TFT, ST7701 controller, 480×640, RGB parallel interface           |
| Touch                 | GT911 capacitive touch controller, I2C                                 |
| Weight Sensor         | HX711 load cell amplifier + 750 g load cell                            |
| Grinder Control       | Solid-State Relay (SSR), active HIGH                                   |
| Onboard extras        | QMI8658 IMU, PCF85063 RTC, TCA9554 GPIO expander, SD card slot, buzzer |
| Development Toolchain | PlatformIO                                                             |
| UI Framework          | LVGL **9.x**                                                           |

---

## 3. Pin Assignments

### 3.4 Project-Specific Pins

| Signal      | GPIO | Notes                                                      |
| ----------- | ---- | ---------------------------------------------------------- |
| HX711 DATA  | 4    | Free GPIO                                                  |
| HX711 CLK   | 44   | UART0 RXD — fully reclaimed (UART RX unused)               |
| SSR control | 43   | UART0 TXD — freed at boot via `uart_set_pin` remap; active HIGH |

---

## 4. Core Functional Requirements

### 4.1 Weight Acquisition

- Continuously read weight from HX711 at ≥10 Hz
- Auto-tare before each grind cycle
- Display live weight in grams with 1 decimal place precision
- 750 g load cell — well above the 1–100 g dose range

### 4.2 Target Weight Selection

- User selects a **preset** from the main screen (highlighted with arc)
- Up to 6 presets, persisted in NVS
- Default presets: 18.0 g, 21.0 g
- `[+]` pill hidden when 6 presets reached

### 4.3 Grind Cycle

1. User taps a preset pill (highlights it with arc) then presses **GRIND**
2. System auto-tares the scale
3. SSR energised (GPIO43 HIGH) → grinder on
4. Live weight shown inside the GRIND button; STOP button appears bottom-right
5. When `live_weight >= (target_weight - pre_stop_offset)` → SSR de-energised
6. Final weight recorded (+ simulated overshoot in demo mode)
7. Button briefly shows final weight (~2 s), then resets to **GRIND**
8. Post-grind toast displayed with result and offset adjustment

### 4.4 Pre-Stop Offset & Auto-Tune

- Configurable pre-stop offset (default 0.3 g, adjustable in Settings in 0.1 g steps)
- After each shot: `delta = final_weight - target_weight`
- Offset auto-adjusted: `new_offset = offset + (delta * AUTOTUNE_FACTOR)`
- `AUTOTUNE_FACTOR = 0.5` — converges in ~3 shots
- Post-grind toast shows the adjustment made
- Auto-tune active in v1; offset clamped to 0–5 g

### 4.5 Relay Control

- GPIO43, active HIGH (UART0 TXD remapped at boot via `uart_set_pin`)
- Always LOW on boot, on any error, and when idle

---

## 5. UI Screens & Flows

### 5.1 Main Screen

**Idle state:**

```
┌─────────────────────────────┐
│  [wifi]                [⚙]  │  ← wifi icon top-left, gear top-right
│                             │
│   ◜18g◝  [21g]  [+]        │  ← selected preset has accent ring
│   ◟   ◞                     │
│                             │
│        ╔═════════╗          │
│        ║         ║          │
│        ║  GRIND  ║          │
│        ║         ║          │
│        ╚═════════╝          │
│                             │
│  [PURGE]                    │  ← always visible bottom-left
└─────────────────────────────┘
```

**Grinding state:**

```
┌─────────────────────────────┐
│  [wifi]                [⚙]  │
│                             │
│   ◜18g◝  [21g]  [+]        │
│   ◟   ◞                     │
│                             │
│        ╔═════════╗          │
│        ║         ║          │
│        ║  14.3g  ║          │  ← live weight replaces GRIND
│        ║         ║          │
│        ╚═════════╝          │
│                             │
│  [PURGE]           [STOP]   │  ← STOP appears bottom-right
└─────────────────────────────┘
```

**Done state** (button shows final weight ~2 s, then resets to GRIND):

```
        ╔═════════╗
        ║  18.1g  ║  → resets to GRIND
        ╚═════════╝
```

- WiFi icon (top-left): tappable, navigates to WiFi screen; colour reflects connection state (accent = connected, dim = disconnected), polled every 1 s
- PURGE button (bottom-left): always visible; triggers a 1500 ms SSR pulse; label changes to "PURGING" while active

### 5.2 Add Preset Flow (`[+]` tapped)

> **Not yet implemented.** The `[+]` button exists and is shown when `count < PRESET_MAX` (6), but `add_preset_cb` is a TODO stub. The panel below is the intended design.

```
┌─────────────────────────────┐
│  [wifi]                [⚙]  │
│                             │
│   ◜18g◝  [21g]             │  ← [+] hidden at max count
│   ◟   ◞                     │
│                             │
│    [ − ]   18.0 g   [ + ]   │  ← 0.1g steps, long-press-repeat
│                             │
│   [Cancel]      [Save]      │
│                             │
└─────────────────────────────┘
```

- GRIND button hidden; panel occupies lower screen
- Default value: last existing preset weight
- Save appends to presets, sets new preset as active
- `[+]` pill hidden when at `PRESET_MAX_COUNT` (6)

### 5.3 Edit / Delete Preset Flow (long press on pill)

> **Not yet implemented.** Intended design:

```
┌─────────────────────────────┐
│  [wifi]                [⚙]  │
│                             │
│   ◜18g◝  [21g]  [+]        │
│   ◟   ◞                     │
│                             │
│    [ − ]   18.0 g   [ + ]   │  ← 0.1g steps, long-press-repeat
│                             │
│   [Cancel]      [Save]      │
│                             │
│         [ Delete ]          │  ← delete at bottom, red text
│                             │
└─────────────────────────────┘
```

- GRIND button hidden
- Current preset value pre-loaded
- Delete shifts remaining presets down, clamps active index
- Float arithmetic uses `roundf` to avoid 0.1 g accumulation drift

### 5.4 Settings Screen (full screen, slides in from right)

```
┌─────────────────────────────┐
│  [←]  Settings              │
│─────────────────────────────│
│                             │
│  WiFi                  [>]  │  ← navigates to WiFi screen
│                             │
│  Calibration           [>]  │
│                             │
│  Firmware Update       [>]  │
│                             │
│  Brightness  ░░░░░░▓▓▓▓▓▓  │  ← slider (10–100%)
│                             │
│  Sleep after  [<] 10 min [>]│  ← options: Never/1/2/5/10/15/30/60 min
│                             │
└─────────────────────────────┘
```

- Brightness and sleep timeout saved to NVS immediately on change (namespace `disp_cfg`)
- Pre-stop offset stepper and Reset to defaults are **not yet implemented**

### 5.5 WiFi Screen (full screen, slides in from right)

**States:**

1. **Scanning** — async `WiFi.scanNetworks(true)`, spinner shown, polls every 250 ms
2. **Results** — networks sorted by RSSI, signal displayed as `▂▄▆█` bars, `[+]` suffix on secured networks
3. **Password modal** — modal overlay with textarea + LVGL keyboard; skipped if saved credentials match
4. **Connecting** — spinner + "Connecting..." status, polls `WiFi.status()` every 250 ms, 15 s timeout
5. **Connected** — connected network shown at top in green with disconnect button

- On connect success: credentials saved to NVS
- On connect failure: error toast shown, server re-scans
- Back button cancels any in-progress operation cleanly

### 5.6 Calibration Wizard (3 steps, slides in from right)

**Step 1 — Clear scale:**

```
┌─────────────────────────────┐
│  [←]  Calibration  1 of 3  │
│    Remove everything from   │
│    the scale and press      │
│    Continue.                │
│        ╔═══════════╗        │
│        ║  0.00 g   ║        │  ← live weight (polls every 100 ms)
│        ╚═══════════╝        │
│                  [Continue] │
└─────────────────────────────┘
```

**Step 2 — Place known weight:**

```
┌─────────────────────────────┐
│  [←]  Calibration  2 of 3  │
│    Place your calibration   │
│    weight on the scale.     │
│        ╔═══════════╗        │
│        ║  99.8 g   ║        │  ← live raw reading
│        ╚═══════════╝        │
│    Known weight:            │
│    [ − ]  100.0 g  [ + ]    │  ← 0.1g steps
│                  [Continue] │
└─────────────────────────────┘
```

**Step 3 — Confirm & save:**

```
┌─────────────────────────────┐
│  [←]  Calibration  3 of 3  │
│    Calibration complete.    │
│    Reading:   99.8 raw      │
│    Known:     100.0 g       │
│    Factor:    1.002         │
│   [Discard]      [Save]     │
└─────────────────────────────┘
```

- Calibration factor = `raw_reading / known_weight`
- Saved to NVS and applied live to HX711

### 5.7 Firmware Update Screen (slides in from right)

**No WiFi state:**

```
┌─────────────────────────────┐
│  [←]  Firmware Update       │
│                             │
│   WiFi not connected.       │
│   Connect via Settings      │
│   > WiFi first.             │
│                             │
│   [ WiFi  Connect to WiFi ] │
└─────────────────────────────┘
```

**WiFi connected state:**

```
┌─────────────────────────────┐
│  [←]  Firmware Update       │
│                             │
│  Open in your browser:      │
│                             │
│    http://192.168.1.42      │  ← large blue text
│                             │
│  Version: v1.0.0            │
│  ─────────────────          │
│  Waiting for upload...      │
│                             │
│  [progress bar hidden]      │
└─────────────────────────────┘
```

**During upload:**

- Progress bar and percentage become visible
- Status label shows "Receiving firmware... 65%"

**On success:**

- Bar fills to 100%, "Update complete — rebooting..." shown
- `ESP.restart()` fires after 2 s

**Implementation:**

- HTTP server (`ota_srv_task`, Core 0, priority 2) starts at boot and is always running
- Upload handled by ESP-IDF `app_update` component (`esp_ota_begin` / `write` / `end`)
- Progress communicated back to LVGL via `volatile` variables polled every 500 ms by a timer in `screen_ota`
- Web page served inline from `web_server.c`; XHR-based upload (no page reload)

### 5.8 Toast Notifications

**Post-grind toast** — bottom of screen, dark green, auto-dismisses after 3 s:

```
╔══════════════════════════════╗
║  Done  ·  18.1g  (+0.1g)    ║
║  Offset adjusted to 0.4g    ║
╚══════════════════════════════╝
```

**Error toasts** — dark red, persist until tapped:

```
╔══════════════════════════════╗  ╔══════════════════════════════╗
║  ⚠ Scale not detected       ║  ║  ⚠ Grind overrun — stopped  ║
╚══════════════════════════════╝  ╚══════════════════════════════╝

╔══════════════════════════════╗  ╔══════════════════════════════╗
║  ⚠ WiFi disconnected        ║  ║  ⚠ Firmware update failed   ║
╚══════════════════════════════╝  ╚══════════════════════════════╝
```

- Toast system implemented inline in `screen_main.c` (no separate widget file)
- Only one toast shown at a time; new toast replaces existing

---

## 6. System Architecture

```
┌──────────────────────────────────────────────────────────┐
│                        ESP32-S3                          │
│                                                          │
│  Core 0                          Core 1                  │
│  ┌─────────────┐                 ┌──────────────────┐    │
│  │  hx711_task │──volatile f32──▶│   app_main loop  │    │
│  │  (prio 5)   │  (real mode     │   lv_timer_handler│   │
│  │  (real mode │   only)         │   grind poll_cb  │    │
│  │   only)     │                 │   SSR control    │    │
│  └─────────────┘                 └──────────────────┘    │
│  ┌──────────────────┐                                    │
│  │  ota_srv_task    │  (persistent, started at boot,     │
│  │  (prio 2)        │   Core 0)                          │
│  └──────────────────┘                                    │
└──────────────────────────────────────────────────────────┘
```

Grind control logic runs as an **LVGL timer** (`poll_cb`, 100 ms) on Core 1 inside the main loop — there is no separate `grind_ctrl_task`. SSR is driven directly from `poll_cb` via `gpio_set_level`. In demo mode (`GRIND_DEMO_MODE=1`, default) weight is simulated; no `hx711_task` is created.

### 6.1 Tasks

| Task           | Core | Priority | Lifetime   | Responsibility                                     |
| -------------- | ---- | -------- | ---------- | -------------------------------------------------- |
| `hx711_task`   | 0    | 5        | Real mode only | Poll HX711 @ 80 Hz, write to `volatile float` |
| `app_main`     | 1    | —        | Always     | LVGL `lv_timer_handler` loop (5 ms tick)           |
| `ota_srv_task` | 0    | 2        | Always     | Persistent HTTP server (OTA + history API)         |

### 6.2 Inter-task Communication

| Primitive                        | Type            | Direction                                        |
| -------------------------------- | --------------- | ------------------------------------------------ |
| `s_latest_weight`                | `volatile float`| hx711_task (writer) → grind poll_cb (reader)     |
| `s_ota_progress` / `s_ota_state` | `volatile` vars | ota_srv_task → LVGL poll timer in screen_ota     |
| Direct function calls            | —               | UI calls `grind_ctrl_start/stop/purge()` directly|

---

## 7. Pre-Stop Offset Logic

```
stop_at_weight = target_weight - pre_stop_offset

if (live_weight >= stop_at_weight):
    deenergise_ssr()
    wait for weight to stabilise (delta < 0.05g for 5 consecutive samples)
    record final_weight
    delta = final_weight - target_weight
    pre_stop_offset = clamp(pre_stop_offset + delta * 0.5, 0.0, 5.0)
    save offset to NVS
    show post-grind toast with delta and new offset
```

---

## 8. Persistence (NVS)

**Namespace: default**

| Key            | Type   | Description                             |
| -------------- | ------ | --------------------------------------- |
| `preset_count` | uint8  | Number of saved presets (default: 2)    |
| `preset_N`     | float  | Weight for preset N (0-indexed)         |
| `offset_g`     | float  | Pre-stop offset (default: 0.3 g)        |
| `cal_factor`   | float  | HX711 calibration factor (default: 1.0) |
| `wifi_ssid`    | string | Last connected SSID                     |
| `wifi_pass`    | string | WiFi password                           |

**Namespace: `disp_cfg`**

| Key           | Type  | Description                                       |
| ------------- | ----- | ------------------------------------------------- |
| `brightness`  | uint8 | Display brightness 10–100 (default: 80)           |
| `timeout_min` | uint8 | Sleep timeout in minutes; 0 = never (default: 10) |

**Namespace: `grind_hist`**

| Key       | Type  | Description                                    |
| --------- | ----- | ---------------------------------------------- |
| `count`   | uint16| Number of records stored (0–50)                |
| `head`    | uint16| Circular buffer head index                     |
| `records` | blob  | Array of `grind_record_t` {target_g, result_g} |

---

## 9. WiFi Features

- **NTP time sync** — sync PCF85063 RTC on WiFi connect (NTP server: `pool.ntp.org`)
- **OTA firmware update** — ESP32 hosts HTTP server on port 80; user uploads `.bin` via browser on same network

---

## 10. PlatformIO Configuration

```ini
; Host-native unit tests — no hardware needed
; Run with: pio test -e native
[env:native]
platform = native
build_src_filter = -<*>
build_flags =
    -std=c11
    -Wall
    -Wno-unused-function
    -I src/core
    -lm

[env:waveshare_28b]
platform = espressif32@6.12.0
board = esp32-s3-devkitc-1
framework = espidf

monitor_speed = 115200
upload_speed = 921600

board_build.flash_size = 16MB
board_build.flash_mode = qio
board_build.partitions = partitions.csv
```

LVGL 9.x is included as a local component under `components/lvgl__lvgl/`. HX711 is not a library dependency — the driver will be implemented directly in `grind_controller.c` when hardware is available.

`build_src_filter = -<*>` in the native env prevents any `src/` files from being compiled automatically. Each test suite in `test/` symlinks directly to the source file(s) it needs, so only the module under test is compiled alongside the suite's stubs.

### LVGL Config Notes (`components/lv_conf.h`)

- Dark theme enabled
- Custom malloc/free using standard heap (PSRAM used for RGB frame buffers via bounce buffer)

---

## 11. Project Structure

```
rocky-3000/
├── platformio.ini
├── partitions.csv
├── DESIGN.md
├── CMakeLists.txt           # ESP-IDF project root
├── sdkconfig.defaults
├── sdkconfig.waveshare_28b  # Board-specific ESP-IDF config (PSRAM, 240 MHz)
├── components/
│   └── lvgl__lvgl/          # LVGL 9.x as a local ESP-IDF component
│       └── lv_conf.h
├── test/
│   ├── unity/
│   │   └── unity.h               # Minimal Unity-compatible framework (header-only)
│   ├── test_grind_history/
│   │   ├── test_grind_history.c  # 10 tests: init, record, ordering, wrap, clear, get
│   │   ├── grind_history.c       # → symlink to src/core/grind_history.c
│   │   ├── grind_history.h       # → symlink to src/core/grind_history.h
│   │   ├── unity.h               # → symlink to test/unity/unity.h
│   │   ├── nvs_stub.c            # NVS stub: writes no-op, reads return NOT_FOUND
│   │   ├── nvs.h                 # ESP-IDF NVS stub header
│   │   ├── nvs_flash.h           # ESP-IDF NVS flash stub header
│   │   └── esp_err.h             # ESP error code stubs
│   └── test_autotune/
│       ├── test_autotune.c       # 9 tests: offset math, deadband, clamp, convergence
│       └── unity.h               # → symlink to test/unity/unity.h
└── src/
    ├── main.c               # Entry point: hw init, wifi, grind ctrl, HTTP server, LVGL loop
    ├── CMakeLists.txt       # Registers all source files as ESP-IDF component
    ├── LCD_Driver/          # ST7701S RGB panel driver
    ├── LVGL_Driver/         # LVGL display + flush integration
    ├── Touch_Driver/        # GT911 capacitive touch + LVGL input device
    ├── I2C_Driver/          # I2C master (GPIO7/15, 400 kHz)
    ├── EXIO/                # TCA9554 GPIO expander (backlight, buzzer)
    ├── Buzzer/              # Buzzer control
    ├── core/
    │   ├── grind_controller.h/c  # Grind state machine, SSR, demo mode, auto-tune
    │   └── grind_history.h/c     # Shot history (circular buffer, NVS persistence)
    └── ui/
        ├── ui_palette.h          # Color constants and layout values
        ├── display_manager.h/c   # Brightness + sleep timeout (NVS: "disp_cfg")
        ├── screen_main.h/c       # Main screen: presets, grind, purge, toasts
        ├── screen_settings.h/c   # Settings: WiFi, Calibration, OTA, brightness, sleep
        ├── screen_calibration.h/c # 3-step calibration wizard
        ├── screen_wifi.h/c       # WiFi scan/connect/disconnect
        ├── screen_ota.h/c        # OTA firmware update UI
        ├── web_server.h/c        # HTTP server: /ota, /update, /history, /api/history
        └── wifi_portal.h/c       # Background auto-connect + NTP sync
```

---

## 12. Milestones

| #   | Status     | Milestone           | Notes                                                                 |
| --- | ---------- | ------------------- | --------------------------------------------------------------------- |
| 1   | ✅ Done    | Project scaffold    | Compiles clean on ESP-IDF / PlatformIO                                |
| 2   | ⏳ Pending | Display + touch     | ST7701S + GT911 driver written; needs hardware verification           |
| 3   | ⏳ Pending | Weight driver       | HX711 stubs in grind_controller.c; needs wiring and calibration       |
| 4   | ⏳ Pending | SSR control         | GPIO33 code written; needs hardware test                              |
| 5   | ⏳ Pending | Core grind logic    | Grind loop + auto-tune complete; demo mode active; needs hardware     |
| 6   | ✅ Done    | UI — Main screen    | Idle/grinding/done states + WiFi icon + PURGE button                  |
| 7   | ⏳ Pending | UI — Preset flows   | Add/edit/delete panel not yet implemented (callback is TODO stub)     |
| 8   | ✅ Done    | UI — Settings       | Brightness slider, sleep timeout stepper, nav to WiFi/Cal/OTA         |
| 9   | ✅ Done    | UI — Toasts         | Post-grind success toast in screen_main.c                             |
| 10  | ✅ Done    | WiFi + OTA          | Scan/connect/password modal; persistent HTTP OTA + history server     |
| 11  | ✅ Done    | Persistence         | NVS for presets, offset, brightness, sleep timeout, wifi creds, history |
| 12  | ✅ Done    | Shot history        | Circular buffer (50 records), NVS persist, web API + HTML page        |
| 13  | 🔄 Started | Polish & testing    | Host unit tests added (grind_history, auto-tune); hardware integration pending |
| 14  | ⏳ Pending | Flow-rate stop prediction | Replace fixed offset with dynamic latency-based overshoot     |
| 15  | ⏳ Pending | Post-stop pulse refinement | Iterative short pulses to close gap on current shot          |
| 16  | ⏳ Pending | Settling detection  | Replace fixed 200 ms delay with std-dev stability check               |

---

## 13. Planned Grind Control Improvements

> Reference implementation: [jaapp/smart-grind-by-weight](https://github.com/jaapp/smart-grind-by-weight)

The current grind controller uses a simple fixed pre-stop offset with post-shot EMA auto-tune. Three improvements are planned for when real hardware (HX711 + SSR) is verified.

---

### 13.1 Dynamic Flow-Rate-Based Stop Prediction

**Current behaviour:**
```c
float stop_at = s_target - s_offset;  // s_offset fixed, default 0.3 g
```

**Problem:** The grinder coast distance depends on how fast grounds are flowing at cutoff. A freshly burr-set grinder running coarse grinds faster and coasts further than a fine espresso grind. A fixed offset cannot account for this.

**Planned approach:**

Calculate a rolling flow rate (g/s) using a short sliding window over recent HX711 samples, then derive the stop threshold from measured motor latency and current flow rate:

```
flow_rate_g_s  = Δweight / Δtime  (rolling 200 ms window)
coast_g        = (motor_latency_ms / 1000.0) × flow_rate_g_s × COAST_RATIO
stop_at        = target - coast_g
```

`motor_latency_ms` is the total delay from SSR de-energise command to burrs stopping (SSR switching time + motor rundown). Typical range 30–150 ms; stored in NVS, initially set to a conservative default (100 ms).

`COAST_RATIO` (initially 1.0) can be tuned down if systematic overshoot remains after latency is calibrated.

**Implementation notes:**
- Flow rate computed inside `hx711_task` (real mode) using a circular buffer of `{weight, timestamp}` pairs.
- Exposed via `grind_ctrl_get_flow_rate()`.
- `poll_cb` uses the live flow rate for the stop decision instead of the fixed `s_offset`.
- `motor_latency_ms` added to NVS namespace `default` as key `motor_lat_ms` (float, default 0.1).

---

### 13.2 Post-Stop Pulse Refinement

**Current behaviour:** Single-shot stop. If the final settled weight is short of target, the deficit is only corrected on the next shot via auto-tune.

**Problem:** A single missed shot wastes the dose; baristas notice immediately.

**Planned approach:**

After the main grind stops and the scale settles, check the shortfall. If it exceeds a minimum threshold, fire one or more short correction pulses:

```
shortfall = target - settled_weight

while shortfall > PULSE_MIN_G and attempts < PULSE_MAX_ATTEMPTS:
    pulse_ms = (shortfall / flow_rate_g_s) × 1000 × PULSE_FACTOR
    pulse_ms = clamp(pulse_ms, PULSE_MIN_MS, PULSE_MAX_MS)
    energise SSR for pulse_ms
    wait for scale to settle
    shortfall = target - settled_weight
    attempts++
```

Constants (tunable, stored in NVS):
| Constant | Default | Notes |
|---|---|---|
| `PULSE_MIN_G` | 0.15 g | Below this, don't pulse (noise floor) |
| `PULSE_MAX_ATTEMPTS` | 3 | Prevents infinite loop on stuck scale |
| `PULSE_MIN_MS` | 30 ms | Shortest meaningful SSR pulse |
| `PULSE_MAX_MS` | 500 ms | Safety cap |
| `PULSE_FACTOR` | 0.8 | Intentional undershoot per pulse |

**State machine addition:**
```
GRIND_IDLE → GRIND_RUNNING → GRIND_SETTLING → GRIND_PULSING → GRIND_DONE → GRIND_IDLE
```

`GRIND_SETTLING` — waits for scale stability after main stop.
`GRIND_PULSING` — fires correction pulses; loops back to `GRIND_SETTLING` after each.

**UI impact:** The grind button continues showing live weight during pulse refinement. A "Refining…" label (or subtle animation) indicates the extra pulses to the user.

---

### 13.3 Scale Settling Detection

**Current behaviour (real mode):**
```c
vTaskDelay(pdMS_TO_TICKS(200));   /* fixed 200 ms coast settle */
s_result = (float)s_latest_weight;
```

**Problem:** 200 ms is a guess. A light dose on a stiff platform may settle in 80 ms; a heavy moka pot dose may still be oscillating at 300 ms.

**Planned approach:**

Declare settled when the standard deviation of the last N samples falls below a gram threshold:

```c
#define SETTLE_WINDOW_SAMPLES  10     // ~125 ms at 80 Hz
#define SETTLE_STD_DEV_G       0.05f  // 0.05 g RMS threshold

bool is_settled(void) {
    // compute mean and std dev of last SETTLE_WINDOW_SAMPLES from circular buffer
    // return std_dev < SETTLE_STD_DEV_G
}
```

`hx711_task` maintains a fixed-size circular buffer of raw calibrated readings. `is_settled()` is called by `poll_cb` after SSR cutoff instead of waiting a fixed delay. A timeout (1 s) prevents hanging if the scale never settles (e.g. vibrating bench).

**Implementation notes:**
- Circular buffer added to `hx711_task` (real mode only); size `SETTLE_WINDOW_SAMPLES`.
- `grind_ctrl_is_settled()` exposed in the public API.
- Existing `vTaskDelay(200)` call removed and replaced with a polling loop using `is_settled()` + timeout.
- `SETTLE_STD_DEV_G` and `SETTLE_WINDOW_SAMPLES` defined in `grind_controller.h` for easy tuning.
