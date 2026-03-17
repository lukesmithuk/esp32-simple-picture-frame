# Progress Log

## 2026-03-17 — Power Optimisation

- **board_sleep() enabled** before deep sleep — PMIC enters low-power mode,
  cutting all unnecessary rails. DLDO1/DLDO2 confirmed unconnected on schematic.
- **Battery support**: AXP2101 ADC enabled for voltage/percent/charge state.
  Battery status logged on each boot and pushed to server.
- **Battery-only operation verified**: frame runs from LiPo, wakes on schedule,
  fetches images, sleeps correctly.

---

## 2026-03-16 — WiFi Photo Retrieval

### Server (Python/FastAPI on Raspberry Pi)

- Web UI: gallery with thumbnails, upload (JPEG/PNG/WebP), delete, frame dashboard
- Uploads auto-converted to baseline JPEG, resized to 800×480 (Lanczos)
- `/api/next`: per-frame shuffle without repeat
- `/api/status` + `/api/logs`: frame pushes status and incremental logs
- Configurable wake interval from web UI (sent via response headers)
- Image sync on startup: scans images/ directory, adds missing files to DB
- API key authentication, systemd service for production
- 15 tests (database + API)

### Firmware

- New `wifi_fetch` component: WiFi connect (10s timeout), NTP → RTC sync,
  HTTP image fetch, status push, incremental log upload
- WiFi-first flow with SD card fallback
- Server-provided wake interval overrides config.txt
- Config key masking (passwords/keys logged as ****)
- Log rolling with configurable max size
- Real timestamps in logs (system time after RTC init)
- Git-based firmware version via `esp_app_get_description()`
- 16MB flash, NVS init for WiFi, noisy WiFi logs suppressed

---

## 2026-03-14 — Phase 9: Production Loop & Reliability

### What was built

- **applog component** (replaces errlog): buffers early-boot ESP_LOG to 4KB RAM,
  flushes and tees all output to SD card file after mount.
- **config component**: key=value config file reader from SD card. Supports
  comments (#), whitespace trimming, typed getters with defaults.
- **Image shuffle**: history file in image directory tracks shown images.
  Cycles through all before repeating. Prunes history when files are
  added/removed.
- **Configurable wake interval**: reads `wake_interval_hours/minutes/seconds`
  from `/sdcard/config.txt`. Defaults to 1h 0m 0s.
- **Full production cycle verified**: wake → decode → display → sleep → repeat.

---

## 2026-03-14 — Phase 7: JPEG Decode + Scale + CDR + Dither

### What was built

New `image_decode` component implementing the full image pipeline:
JPEG decode (TJpgDec via ESP32-S3 ROM) → nearest-neighbor cover-mode scale to
800×480 → compress dynamic range (CDR) → Floyd-Steinberg dither with measured
Spectra 6 palette → 4bpp packed frame buffer.

Added `espressif/esp_jpeg` as a managed component dependency.

### Bugs found and fixed

- **`epd_color_t` wrong since Phase 4** — indices didn't match the actual panel
  hardware. Corrected to: 0=Black, 1=White, 2=Yellow, 3=Red, 5=Blue, 6=Green.
  The panel is Spectra 6 (E6) with 6 colours — no Orange. Index 4 is reserved.
- **Panel 180° orientation** — panel scans from bottom-right origin. Fixed by
  adding buffer rotation (reverse bytes + swap nibbles) in `epd_display()`.
- **EPD_COLOR_ORANGE removed** — was never a real colour on this panel.

### Known limitations

- **Baseline JPEG only** — progressive JPEG not supported (TJpgDec limitation)
- **No EXIF orientation** — images with EXIF rotation tags display incorrectly
- **No portrait rotation** — portrait images get cropped, not rotated

---

## 2026-03-14 — Phase 6: SD Card + Image Pipeline Skeleton

SD card mount (4-bit SDIO), image picker (random selection), image loader
(file → PSRAM buffer), error text display on EPD, error logging to file.
All verified on hardware. Merged as PR #7.

---

## 2026-03-10 — Phase 5: RTC Alarm + Deep Sleep

PCF85063 alarm implementation, ESP32 EXT0 wakeup on GPIO6, compile-time
RTC init, full sleep/wake cycle. Merged as PR #6.

---

## 2026-03-08 — Phase 4: Architecture + Fresh Implementation

### Context

All previous code deleted on the `start-again` branch (commit `ec2eacd`).
Phases 1–3 are abandoned — the knowledge gained is preserved in DECISIONS.md
and MEMORY.md, but no code from those phases carries forward.

The fresh implementation ports drivers directly from `aitjcize/esp32-photoframe`
(confirmed working on this hardware) rather than rebuilding from scratch.

### Root cause of previous BUSY-stuck bug (Phase 3)

Comparing our previous driver against `aitjcize/driver_ed2208_gca.c`:

```
aitjcize:      0x10 + data → wait_busy("data") → 0x04 PON → wait_busy("power_on") → ...
previous ours: 0x10 + data → 0x04 PON → wait_busy("power_on") → ...  ← missing wait
```

Missing `wait_busy` after data transfer before PON is the suspected root cause.
Also: aitjcize re-inits the panel (full hw_reset + init sequence) on every refresh
call; our previous driver did not.

### Architecture decisions

See `DECISIONS.md` for full ADRs (ADR-001 through ADR-009).

Summary:
- Board HAL component owns I2C (PMIC + RTC); EPD is a separate SPI component
- XPowersLib vendored for AXP2101 PMIC (same as aitjcize, confirmed working)
- PCF85063 RTC driver ported verbatim from aitjcize
- EPD driver adapted from aitjcize: hard-coded pins, same init bytes and sequence
- Two-tier test infrastructure: Unity (pure logic) + TEST_MODE (hardware-in-loop)

### Files created

```
CMakeLists.txt
sdkconfig.defaults
Kconfig.projbuild
PLAN.md
DECISIONS.md
TODO.md
PROGRESS.md  (this file)
main/
  CMakeLists.txt
  main.c
  test_main.h
  test_main.c
components/board/
  CMakeLists.txt
  include/board.h
  axp2101.h
  axp2101.cpp           ← ported from aitjcize (XPowersLib wrapper)
  pcf85063.h
  pcf85063.c            ← ported verbatim from aitjcize
  board.c
  XPowersLib/           ← vendored from aitjcize/pmic_driver_axp2101/src/
    XPowersLib.h
    XPowersLibInterface.cpp / .hpp
    XPowersLib_Version.h
    XPowersParams.hpp
    XPowersCommon.tpp
    XPowersAXP2101.tpp
    XPowersAXP192.tpp
    XPowersAXP202.tpp
    PowersBQ25896.tpp
    PowersSY6970.tpp
    PowerDeliveryHUSB238.hpp
    REG/
      AXP2101Constants.h (+ others)
components/epd/
  CMakeLists.txt
  include/epd.h
  epd.c                 ← adapted from aitjcize driver_ed2208_gca.c
components/board/test/
  CMakeLists.txt
  test_board.c          ← Unity placeholder
components/epd/test/
  CMakeLists.txt
  test_epd.c            ← Unity: epd_fill_color packing tests
```

### Status — VERIFIED ON HARDWARE (2026-03-08)

**Build**: clean, zero errors, zero warnings.

**First flash result**: display updated to solid white ✅

```
I (781) axp2101: Init PMU SUCCESS!        ← PMIC init OK, chip ID 0x4A
I (821) pcf85063_rtc: PCF85063ATL RTC initialized successfully  ← RTC OK
I (841) axp2101: EPD power ON (ALDO3)
I (841) epd: EPD init complete
I (991) epd: Sending 192000 bytes in 128-byte chunks
I (1111) epd: Buffer send complete
[display updated to white]
```

**BUSY-stuck bug confirmed fixed.** Root cause was the missing `wait_busy("data")`
between the data transfer and `0x04 PON`. The fix (porting aitjcize's sequence
exactly) resolved it on the first attempt.

Note: monitor timeout was 30s — the full refresh cycle (wait_busy × 4 + e-paper
scan time) takes longer. Use `--timeout 120` to capture the complete log.

### Next steps

- Flash with `TEST_MODE=y` to exercise all 6 palette colours
- Phase 5: RTC alarm + deep sleep wake cycle

### Expected verification sequence

1. `idf.py build` — clean compile
2. Flash TEST_MODE build
3. Verify PMIC init (chip ID 0x4A logged), ALDO3 on/off
4. Verify RTC init (PCF85063 detected)
5. Verify EPD solid white — if BUSY clears, the root-cause fix is confirmed
6. Verify all 6 palette colours

---

## Historical — Phases 1–3 (abandoned 2026-03-08)

All code from phases 1–3 has been deleted. Key findings are preserved in
DECISIONS.md and MEMORY.md.

- **Phase 1** (2026-02-21): Hardware bring-up. GPIO map confirmed from schematic.
  SD card 4-bit SDIO verified. I2C bus + device addresses confirmed.
- **Phase 2** (2026-02-22): AXP2101 pure-C PMIC driver. Write test passed.
  Chip ID 0x4A confirmed. I2C pattern confirmed (persistent device handle,
  `transmit_receive` is synchronous).
- **Phase 3** (2026-02-22 – 2026-03-02): EPD driver. Code-complete but never
  displayed an image. BUSY-stuck bug after 0x04 PON. Root cause identified
  post-mortem (see above). Branch `phase3-epd-driver` abandoned.
