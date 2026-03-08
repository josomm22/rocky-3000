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
| UI Framework          | LVGL **8.4.0** (resolved from `^8.3.10`)                               |

---

## 3. Pin Assignments

### 3.4 Project-Specific Pins

| Signal      | GPIO | Notes                  |
| ----------- | ---- | ---------------------- |
| HX711 DATA  | 33   | Free GPIO              |
| HX711 CLK   | 34   | Free GPIO              |
| SSR control | 35   | Free GPIO, active HIGH |

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
3. SSR energised (GPIO35 HIGH) → grinder on
4. Live weight shown inside the GRIND button; STOP button appears bottom-left
5. When `live_weight >= (target_weight - pre_stop_offset)` → SSR de-energised
6. Weight stabilises; final weight recorded
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

- GPIO35, active HIGH
- Always LOW on boot, on any error, and when idle

---

## 5. UI Screens & Flows

### 5.1 Main Screen

**Idle state:**

```
┌─────────────────────────────┐
│                        [⚙]  │
│                             │
│   ◜18g◝  [21g]  [+]        │  ← selected preset has arc
│   ◟   ◞                     │
│                             │
│        ╔═════════╗          │
│        ║         ║          │
│        ║  GRIND  ║          │
│        ║         ║          │
│        ╚═════════╝          │
│                             │
└─────────────────────────────┘
```

**Grinding state:**

```
┌─────────────────────────────┐
│                        [⚙]  │
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
│  [STOP]                     │  ← appears bottom-left
└─────────────────────────────┘
```

**Done state** (button shows final weight ~2 s, then resets to GRIND):

```
        ╔═════════╗
        ║  18.1g  ║  → resets to GRIND
        ╚═════════╝
```

### 5.2 Add Preset Flow (`[+]` tapped)

```
┌─────────────────────────────┐
│                        [⚙]  │
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

```
┌─────────────────────────────┐
│                        [⚙]  │
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
│  Pre-stop offset            │
│  [ − ]   0.3 g   [ + ]      │  ← 0.1g steps
│                             │
│  Brightness                 │
│  ░░░░░░░░░░▓▓▓▓▓▓  75%     │  ← slider
│                             │
│  Calibration           [>]  │
│                             │
│  Firmware Update       [>]  │
│                             │
│      [ Reset to defaults ]  │
└─────────────────────────────┘
```

- All changes saved to NVS on back navigation

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

- HTTP server runs in a dedicated FreeRTOS task on Core 0 (`ota_srv`, 6 KB stack)
- Upload handled by Arduino `Update` library (`Update.begin` / `write` / `end`)
- Progress communicated back to LVGL via `volatile` variables polled every 500 ms
- Web page served from PROGMEM with XHR-based upload (no page reload)
- Back button stops the server task cleanly

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

- Toast system implemented in `ui_manager.cpp` (no separate widget file)
- Only one toast shown at a time; new toast replaces existing

---

## 6. System Architecture

```
┌──────────────────────────────────────────────────────────┐
│                        ESP32-S3                          │
│                                                          │
│  Core 0                          Core 1                  │
│  ┌─────────────┐                 ┌──────────────────┐    │
│  │  hx711_task │──xQueueWeight──▶│   lvgl_task      │    │
│  │  (prio 5)   │                 │   ui_logic       │    │
│  └─────────────┘                 └──────────────────┘    │
│  ┌──────────────────┐                                    │
│  │  grind_ctrl_task │◀──xEventGroupGrind──────────────── │
│  │  (prio 5)        │                                    │
│  └────────┬─────────┘                                    │
│           │ GPIO35 (SSR)                                 │
│  ┌──────────────────┐                                    │
│  │  ota_srv_task    │  (spawned only when OTA screen      │
│  │  (prio 2)        │   is open, Core 0)                 │
│  └──────────────────┘                                    │
└──────────────────────────────────────────────────────────┘
```

### 6.1 Tasks

| Task              | Core | Priority | Lifetime        | Responsibility                         |
| ----------------- | ---- | -------- | --------------- | -------------------------------------- |
| `hx711_task`      | 0    | 5        | Always          | Poll HX711, push readings to queue     |
| `grind_ctrl_task` | 0    | 5        | Always          | Consume weight, control SSR, auto-tune |
| `lvgl_task`       | 1    | 2        | Always          | LVGL tick + display flush              |
| `ota_srv_task`    | 0    | 2        | OTA screen only | HTTP server, firmware write            |

### 6.2 Inter-task Communication

| Primitive                        | Type                   | Direction                                      |
| -------------------------------- | ---------------------- | ---------------------------------------------- |
| `xQueueWeight`                   | Queue (float, depth 8) | hx711_task → grind_ctrl_task                   |
| `xGrindEventGroup`               | EventGroup             | UI → grind_ctrl_task (START/STOP)              |
| `xLvglMutex`                     | Mutex                  | Guards all `lv_*` calls from multiple contexts |
| `s_ota_state` / `s_ota_progress` | `volatile` vars        | ota_srv_task → LVGL timer                      |

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

| Key            | Type   | Description                             |
| -------------- | ------ | --------------------------------------- |
| `preset_count` | uint8  | Number of saved presets (default: 2)    |
| `preset_N`     | float  | Weight for preset N (0-indexed)         |
| `offset_g`     | float  | Pre-stop offset (default: 0.3 g)        |
| `cal_factor`   | float  | HX711 calibration factor (default: 1.0) |
| `brightness`   | uint8  | Display brightness 0–255 (default: 200) |
| `wifi_ssid`    | string | Last connected SSID                     |
| `wifi_pass`    | string | WiFi password                           |

---

## 9. WiFi Features

- **NTP time sync** — sync PCF85063 RTC on WiFi connect (NTP server: `pool.ntp.org`)
- **OTA firmware update** — ESP32 hosts HTTP server on port 80; user uploads `.bin` via browser on same network

---

## 10. PlatformIO Configuration

```ini
[env:esp32s3]
platform = espressif32
board = esp32s3box
framework = arduino
board_build.arduino.memory_type = qio_opi
board_upload.flash_size = 16MB

lib_deps =
    lvgl/lvgl @ ^8.3.10     ; resolves to 8.4.0
    bogde/HX711 @ ^0.7.5

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DLV_CONF_INCLUDE_SIMPLE
    -DCORE_DEBUG_LEVEL=3
    -I include               ; required for LVGL to find lv_conf.h

monitor_speed = 115200
monitor_filters = esp32_exception_decoder
```

### Display Driver Notes

- ST7701 RGB parallel interface via `esp_lcd_rgb_panel` (esp-idf 5.1.x)
- Clock source: `LCD_CLK_SRC_PLL160M`
- **Single** frame buffer allocated in PSRAM (`flags.fb_in_psram = 1`)
- `num_fbs` and `bounce_buffer_size_px` fields do **not** exist in esp-idf 5.1.x
- LVGL draw buffer: half-screen (`LCD_WIDTH * LCD_HEIGHT / 2` pixels) in PSRAM
- RGB data pin order: D0=NC(B0), D1–D4=B1–B4, D5–D10=G0–G5, D11–D15=R1–R5

### Touch Driver Notes

- GT911 on I2C (GPIO7/15), INT on GPIO16, RST via TCA9554 EXIO2
- I2C address: 0x5D (INT held LOW at boot)
- LVGL input device registered via `lv_indev_drv_t`

### LVGL Config Notes

- `LV_USE_BTNMATRIX 1` required — `LV_USE_KEYBOARD` and `LV_USE_MSGBOX` depend on it
- Dark theme enabled (`LV_THEME_DEFAULT_DARK 1`)
- Custom malloc/free using standard heap (PSRAM used directly for frame buffers)

---

## 11. Project Structure

```
GBWUI/
├── platformio.ini
├── DESIGN.md
├── include/
│   ├── pins.h              # All GPIO definitions
│   ├── config.h            # Compile-time constants (weights, timing, tuning)
│   └── lv_conf.h           # LVGL 8.4.x configuration
└── src/
    ├── main.cpp             # Entry point, FreeRTOS task creation, SSR safe-off
    ├── drivers/
    │   ├── display.h/cpp    # ST7701 RGB panel + LVGL flush callback
    │   ├── touch.h/cpp      # GT911 I2C driver + LVGL pointer input device
    │   └── hx711_scale.h/cpp # HX711 task, tare, calibration factor
    ├── core/
    │   ├── grind_controller.h/cpp  # Grind loop, SSR control, auto-tune offset
    │   └── nvs_config.h/cpp        # NVS persistence, AppConfig struct
    └── ui/
        ├── ui_manager.h/cpp    # LVGL task, dark theme, toast system
        └── screens/
            ├── screen_main.h/cpp         # Main screen + preset add/edit/delete panel
            ├── screen_settings.h/cpp     # Settings screen
            ├── screen_calibration.h/cpp  # 3-step calibration wizard
            ├── screen_wifi.h/cpp         # WiFi scan/connect/disconnect
            └── screen_ota.h/cpp          # OTA firmware upload via HTTP
```

---

## 12. Milestones

| #   | Status     | Milestone         | Notes                                                                    |
| --- | ---------- | ----------------- | ------------------------------------------------------------------------ |
| 1   | ✅ Done    | Project scaffold  | Compiles clean; 42% flash used                                           |
| 2   | ⏳ Pending | Display + touch   | Needs hardware — ST7701 RGB driver written, GT911 stub needs TCA9554 RST |
| 3   | ⏳ Pending | Weight driver     | HX711 code written; needs hardware calibration                           |
| 4   | ⏳ Pending | SSR control       | GPIO code written; needs hardware test                                   |
| 5   | ⏳ Pending | Core grind logic  | Grind loop + auto-tune written; needs hardware                           |
| 6   | ✅ Done    | UI — Main screen  | All 3 states implemented                                                 |
| 7   | ✅ Done    | UI — Preset flows | Add, edit, delete with stepper panel                                     |
| 8   | ✅ Done    | UI — Settings     | Offset stepper, brightness slider, calibration wizard                    |
| 9   | ✅ Done    | UI — Toasts       | Success + error toasts in ui_manager                                     |
| 10  | ✅ Done    | WiFi + OTA        | Scan/connect/password modal; HTTP OTA server                             |
| 11  | ✅ Done    | Persistence       | NVS save/load for all settings and presets                               |
| 12  | ⏳ Pending | Polish & testing  | Hardware-dependent: offset tuning, edge cases                            |
