# Architecture Decisions

Record of key decisions made during the project, with rationale.

---

## ADR-001: Toolchain — ESP-IDF v6.0.0, no Arduino

**Decision**: Use ESP-IDF v6.0.0 directly via EIM. No Arduino framework.

**Rationale**: Full control over power management, sleep modes, and peripheral initialisation.
Arduino abstractions add overhead and hide the register-level control needed for deep-sleep tuning.

**Status**: Decided

---

## ADR-002: Language — pure C

**Decision**: Pure C only. XPowersLib (C++) will not be used — port PMIC logic to C directly.

**Rationale**: After Phase 1 bring-up confirmed TG28 is AXP2101 register-compatible, the
XPowersLib dependency is unnecessary. The only PMIC operations needed (init, sleep, shutdown)
are a small set of register writes that are straightforward to port from the XPowersAXP2101.tpp
template into plain C functions. Introducing a C++ template library dependency for a handful
of I2C writes is not worth the added complexity.

**Status**: Decided (revised after Phase 1 hardware confirmation)

---

## ADR-003: Image dithering — Floyd-Steinberg with measured palette

**Decision**: Use Floyd-Steinberg error diffusion dithering against a *measured* Spectra 6 palette
(actual RGB values of pigments as rendered by the panel), not theoretical RGB values.

**Rationale**: The Waveshare official Jan 2026 code uses theoretical pure-sRGB palette
`{0,0,0}, {255,255,255}, {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}`. The aitjcize project
documents that this produces inferior results vs. a palette measured off the actual panel.
Must locate measured values in aitjcize epaper component or measure directly.

**Note**: Waveshare's FS dithering working buffer bug: `malloc()` for 1.15 MB (800×480×3) will
fail silently or crash. Must use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` instead.

**Status**: Proposed — measured palette values still TBD; source from aitjcize epaper component

---

## ADR-004: Framebuffer — PSRAM, 4bpp packed

**Decision**: Allocate the 800×480 framebuffer in the 8 MB PSRAM at 4 bits per pixel.

**Rationale**: 800 × 480 × 4bpp = 192 KB, comfortably fits in PSRAM. 4bpp supports 16 palette
indices; only 6 are used but 4bpp is a natural packing unit for the Spectra 6 protocol.

**Status**: Proposed

---

## ADR-005: Image source — SD card rotation (primary), WiFi fetch (later)

**Decision**: Initial implementation reads images sequentially from microSD (FAT32). WiFi fetch
added as a later phase.

**Rationale**: Simplest path to a working device. SD card eliminates WiFi power cost for the
common case. WiFi adds complexity and wake-time power draw.

**Status**: Decided

---

## ADR-006: Wakeup mechanism — PCF85063 RTC alarm

**Decision**: Use the PCF85063 RTC alarm output to wake the ESP32-S3 from deep sleep, rather than
the internal ULP timer.

**Rationale**: The internal timer loses context across power cycles and has limited accuracy.
The PCF85063 has a dedicated backup battery header and maintains time/alarms independently of
the main supply. Enables true daily-at-fixed-time scheduling.

**Implementation detail**: The aitjcize firmware wakes the ESP32-S3 via the AXP2101/TG28 IRQ pin,
not a direct PCF85063 INTB → GPIO connection. The likely path is:
`PCF85063 INTB → TG28 IRQ input → TG28 IRQ output → ESP32-S3 wakeup pin`.
TG28 is confirmed AXP2101 register-compatible (ADR-007), so this chain should work.
An alternative is a direct PCF85063 INTB wire to a free ESP32-S3 GPIO (EXT0/EXT1 wakeup).
Must verify from schematic before choosing approach.

**Also**: The aitjcize PCF85063 driver does NOT implement alarm registers. Alarm support (registers
0x0B–0x0F, each with AEN bit) must be written from scratch regardless of which wakeup path is used.

**Status**: Proposed — schematic verification required for wakeup pin routing

---

## ADR-007: TG28 PMIC — AXP2101 register-compatible; implement in pure C

**Decision**: Port AXP2101 sleep/init logic to pure C, referencing aitjcize axp2101_basic_sleep_start
and the XPowersAXP2101.tpp register map. Patch chip ID check to accept 0x4A (TG28) in addition to
0x47 (AXP2101).

**Rationale**: Phase 1 bring-up (2026-02-21) confirmed:
- TG28 responds at I2C 0x34 ✓
- Registers 0x00–0x41 are all readable ✓
- IRQ_EN_1 (0x40) write/readback/restore test PASSED ✓
- Chip ID register 0x03 = 0x4A (not 0x47, but register map is otherwise compatible)

The aitjcize XPowersLib C++ dependency is avoided per ADR-002. The key PMIC operations for this
project are a small set: identify chip, configure sleep rail state, set wakeup source. These are
straightforward to implement as C functions with direct I2C register writes.

**Status**: Decided — Phase 1 complete, PMIC driver implementation unblocked

---

## ADR-008: Reference firmware — aitjcize/esp32-photoframe + Waveshare Jan 2026

**Decision**: Use both references for different subsystems:
- **aitjcize**: component structure, board HAL pattern, PCF85063 driver (adapt), PMIC sleep logic
- **Waveshare Jan 2026**: EPD init byte sequence (authoritative), image decode pipeline (adapt)

**Rationale**: After reading source code, the split is clear:
- aitjcize has clean ESP-IDF component architecture and good sleep/wakeup patterns but its PMIC
  driver is C++ (XPowersLib) and its PCF85063 driver lacks alarm support.
- Waveshare Jan 2026 has the authoritative EPD init sequence and working JPG/PNG/BMP decode with
  scaling and FS dithering — but uses theoretical palette and has a PSRAM allocation bug.
- Neither source handles TG28 at all.

**Status**: Decided (refined after source code review)

---

## ADR-009: JPEG decoder — esp_jpeg (ESP-IDF built-in)

**Decision**: Use `esp_jpeg` component (`esp_jpeg_decode_one_picture()`) for JPEG decoding.

**Rationale**: Confirmed in Waveshare Jan 2026 firmware. Already in ESP-IDF, no external dependency,
outputs RGB888. Tested on this hardware class.

**Status**: Decided

---

## ADR-010: Image scaling — bilinear interpolation, fixed-point

**Decision**: Use bilinear interpolation for resize (not nearest-neighbour).

**Rationale**: Waveshare's `ImgDecode_ScaleRgb888Nearest` is actually bilinear despite its name
(uses 4-neighbour weighted average with fixed-point ×1024 arithmetic). Quality is materially better
than true nearest-neighbour for e-ink where dithering amplifies blocky scaling artefacts.
The fixed-point implementation is fast enough without floating-point.

**Status**: Decided

---

## ADR-011: SD card interface — verify SDIO vs SPI

**Decision**: Pending — verify from schematic and board HAL pin definitions.

**Rationale**: aitjcize board HAL references SDIO pins (CLK, CMD, D0–D3). Waveshare demo may use
SPI-mode SD. Our project should use whichever the hardware is wired for. If SDIO, use
`sdmmc_host_t` with `SDMMC_HOST_DEFAULT()`; if SPI, use `esp_vfs_fat` + SPI SD driver.

**Status**: Blocked — verify from schematic
