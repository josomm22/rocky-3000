# GBWUI — Grind By Weight UI

ESP32-S3 firmware for a coffee grinder controller. Monitors live weight via HX711, controls the grinder through an SSR, and presents a touch UI on a 2.8" capacitive LCD.

**Board:** Waveshare ESP32-S3-Touch-LCD-2.8B

## Features

- Up to 6 weight presets, persisted across reboots
- Auto-tare before each grind
- Pre-stop offset with auto-tune (converges in ~3 shots)
- Shot history (last 50 grinds, viewable via browser)
- OTA firmware updates over WiFi
- 3-step scale calibration wizard
- Display brightness + sleep timeout settings

## Build & Flash

```bash
# Build
pio run -e waveshare_28b

# Build + flash
pio run -e waveshare_28b -t upload

# Serial monitor (115200 baud)
pio device monitor -b 115200
```

## OTA Updates

1. Connect the device to WiFi via Settings → WiFi
2. Go to Settings → Firmware Update
3. Open the displayed IP address in a browser
4. Upload a `.bin` file built with `pio run -e waveshare_28b`

The binary is at `.pio/build/waveshare_28b/firmware.bin`.

## Unit Tests

Host-native, no hardware needed:

```bash
pio test -e native
```

Covers grind history (circular buffer) and auto-tune offset math.

## Wiring

| Signal     | GPIO | Notes                                      |
|------------|------|--------------------------------------------|
| HX711 DATA | 4    |                                            |
| HX711 CLK  | 44   |                                            |
| SSR        | 43   | Active HIGH; UART0 TX remapped at boot     |
| I2C SCL    | 7    | GT911 touch + TCA9554 expander             |
| I2C SDA    | 15   |                                            |

## Versioning

Version is in `src/version.h`. To release:

```bash
# Bump APP_VERSION_MAJOR/MINOR/PATCH in src/version.h, then:
git tag v1.x.x && git push --tags
```
