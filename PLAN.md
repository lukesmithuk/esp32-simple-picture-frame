# Project Plan — ESP32-S3 Picture Frame

## Goal

ESP32-S3 e-paper picture frame on the Waveshare ESP32-S3-PhotoPainter board.

Wake from deep sleep on RTC alarm → fetch image from WiFi server (or SD card
fallback) → dither to 6-colour e-paper palette → render to display → PMIC
sleep → deep sleep until next alarm.

---

## Architecture

```
app_main (main/)
  ├── TEST_MODE path  → tests_run()           [main/test_main.c]
  └── Production path
        ├── board_init()                      [components/board/]
        │     ├── I2C bus recovery (bit-banged)
        │     ├── axp2101_init() + cmd_init() [board/axp2101.cpp — XPowersLib]
        │     └── pcf85063_init()             [board/pcf85063.c]
        ├── board_epd_power(true)             [→ AXP2101 ALDO3 enable]
        ├── epd_init()                        [components/epd/]
        ├── sdcard_mount()                    [components/sdcard/]
        ├── config_load()                     [components/config/]
        ├── try_wifi_fetch()                  [components/wifi_fetch/]
        │     ├── WiFi connect (10s timeout)
        │     ├── NTP sync → RTC update
        │     ├── Upload logs + push status
        │     ├── GET /api/next → img_buf
        │     └── WiFi disconnect
        ├── (fallback) image_picker + loader  [SD card if WiFi fails]
        ├── image_decode_jpeg()               [components/image_decode/]
        │     ├── JPEG decode (TJpgDec ROM)
        │     ├── Scale (cover + centre-crop)
        │     ├── CDR (compress dynamic range)
        │     └── Floyd-Steinberg dither → 4bpp
        ├── epd_display(frame_buf)            [180° rotate → SPI transfer]
        ├── sdcard_unmount()
        ├── epd_deinit()
        ├── board_epd_power(false)
        ├── board_sleep()                     [PMIC low-power mode]
        └── board_enter_deep_sleep()          [RTC alarm → EXT0 wake]
```

### Component summary

| Component | Language | Role |
|-----------|----------|------|
| `components/board/` | C + C++ | I2C bus (bit-banged), PMIC (XPowersLib), RTC |
| `components/board/XPowersLib/` | C++ (vendored) | AXP2101 PMIC register access |
| `components/epd/` | C | SPI, EPD panel driver (Spectra 6, 6-colour) |
| `components/sdcard/` | C | 4-bit SDIO mount/unmount |
| `components/image_picker/` | C | Directory scan + shuffle without repeat |
| `components/image_loader/` | C | File → PSRAM buffer (4MB max) |
| `components/image_decode/` | C | JPEG decode → scale → CDR → dither |
| `components/epd_text/` | C | 8x8 bitmap font for error messages |
| `components/applog/` | C | ESP_LOG tee to SD card file + explicit log writes |
| `components/config/` | C | Key=value config file reader from SD card |
| `components/wifi_fetch/` | C | WiFi, NTP, HTTP image/status/log client |
| `server/` | Python | FastAPI photo server with web UI |
| `main/` | C | App entry point, integration tests |

### Test infrastructure

- **Hardware integration tests** (`TEST_MODE` kconfig): `main/test_main.c`
  Runs via normal flash. Tests board init, PMIC power toggle, RTC availability,
  and EPD solid-colour display for each of the 6 palette colours.
- **Unity component tests** (`idf.py -T`): `components/*/test/`
  Pure logic only (no hardware). Currently: EPD pixel-packing tests.

---

## Phase Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Hardware bring-up & schematic analysis | ✅ Done (2026-02-21) |
| 2 | PMIC driver (AXP2101) | ✅ Done (2026-02-22) |
| 3 | EPD driver — previous attempt | ❌ Abandoned (BUSY-stuck bug) |
| 4 | Architecture definition + fresh implementation | ✅ Done (2026-03-08) |
| 5 | RTC alarm + deep sleep wake cycle | ✅ Done (2026-03-10) |
| 6 | SD card mount + image selection + error display | ✅ Done (2026-03-14) |
| 7 | JPEG decode + scale + CDR + dither | ✅ Done (2026-03-14) |
| 8 | Image quality & server processing | ⬜ Planned |
| 9 | Production loop & reliability | ✅ Done (2026-03-14) |
| — | WiFi photo retrieval (server + firmware) | ✅ Done (2026-03-17) |
| 10 | Power optimisation (PMIC sleep, battery) | ✅ Done (2026-03-17) |
| 11 | Multi-frame (naming, galleries, wake interval) | ✅ Done (2026-04-12) |

---

## Reference Implementations

- `aitjcize/esp32-photoframe` — primary reference; confirmed working on this hardware.
  Sparse-cloned to `/tmp/esp32-photoframe-ref/` (not committed here).
- `waveshareteam/ESP32-S3-PhotoPainter` — authoritative EPD init bytes (cross-checked).
