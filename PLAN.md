# Project Plan — ESP32-S3 Picture Frame

## Goal

ESP32-S3 e-paper picture frame on the Waveshare ESP32-S3-PhotoPainter board.

Wake from deep sleep on RTC alarm → load image from SD card → dither to
6-colour e-paper palette → render to display → sleep until next alarm.

---

## Architecture

```
app_main (main/)
  ├── TEST_MODE path  → tests_run()           [main/test_main.c]
  └── Production path
        ├── board_init()                      [components/board/]
        │     ├── I2C bus recovery
        │     ├── i2c_new_master_bus()
        │     ├── axp2101_init() + cmd_init() [board/axp2101.cpp — XPowersLib]
        │     └── pcf85063_init()             [board/pcf85063.c  — ported from aitjcize]
        ├── board_epd_power(true)             [→ AXP2101 ALDO3 enable]
        ├── epd_init()                        [components/epd/]
        ├── epd_display(frame_buf)            [hw_reset → init → data → refresh]
        ├── epd_deinit()
        ├── board_epd_power(false)
        └── TODO: board_sleep() + esp_deep_sleep_start()
```

### Component summary

| Component | Language | Role |
|-----------|----------|------|
| `components/board/` | C + C++ | I2C bus, PMIC (XPowersLib), RTC |
| `components/board/XPowersLib/` | C++ (vendored) | AXP2101 PMIC register access |
| `components/epd/` | C | SPI, EPD panel driver |
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
| **4** | **Architecture definition + fresh implementation** | 🔄 In progress |
| 5 | RTC alarm + deep sleep wake cycle | ⬜ Planned |
| 6 | SD card mount + image selection | ⬜ Planned |
| 7 | JPEG/PNG decode + bilinear scale | ⬜ Planned |
| 8 | Floyd-Steinberg dither to 6-colour palette | ⬜ Planned |
| 9 | Production loop (alarm → image → display → sleep) | ⬜ Planned |
| 10 | Power optimisation (pmic_sleep, DLDO mapping) | ⬜ Planned |

---

## Reference Implementations

- `aitjcize/esp32-photoframe` — primary reference; confirmed working on this hardware.
  Sparse-cloned to `/tmp/esp32-photoframe-ref/` (not committed here).
- `waveshareteam/ESP32-S3-PhotoPainter` — authoritative EPD init bytes (cross-checked).
