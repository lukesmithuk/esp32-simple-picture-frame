# Architecture Decisions

Record of key decisions made during the project, with rationale.

---

## ADR-001: Toolchain — ESP-IDF v6.0.0, no Arduino

**Decision**: Use ESP-IDF v6.0.0 directly via EIM. No Arduino framework.

**Rationale**: Full control over power management, sleep modes, and peripheral initialisation.
Arduino abstractions add overhead and hide the register-level control needed for deep-sleep tuning.

**Status**: Decided

---

## ADR-002: Language — C

**Decision**: Implement in C (not C++).

**Rationale**: Developer preference (20+ years C experience). ESP-IDF is a C-native ecosystem.
No object-oriented requirements that justify C++ complexity overhead.

**Status**: Decided

---

## ADR-003: Image dithering — Floyd-Steinberg with measured palette

**Decision**: Use Floyd-Steinberg error diffusion dithering against a *measured* Spectra 6 palette
(actual RGB values of pigments as rendered by the panel), not theoretical RGB values.

**Rationale**: The aitjcize/esp32-photoframe project demonstrated significantly better visual results
using the measured palette. Theoretical pure-RGB values produce washed-out dithering because the
e-ink panel renders colours darker/more muted than their sRGB equivalents.

**Status**: Proposed — measured palette values TBD (source from aitjcize or measure directly)

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

**Status**: Proposed — depends on PCF85063 INTB → GPIO wakeup wiring (verify from schematic)

---

## ADR-007: TG28 PMIC — status unknown, AXP2101 compatibility unverified

**Decision**: Defer PMIC-dependent code until TG28 register map is confirmed.

**Rationale**: No public TG28 datasheet. Board has TG28, not AXP2101. Reading chip ID register
0x03 will confirm compatibility. Until then, avoid assumptions; use GPIO 6 (EPD PWR) for display
power, which is independent of the PMIC.

**Status**: Blocked — requires hardware verification (Phase 1)

---

## ADR-008: Reference firmware — aitjcize/esp32-photoframe

**Decision**: Use aitjcize/esp32-photoframe as the primary code reference for EPD driver, dithering,
and deep sleep patterns. Use Waveshare demo for hardware init sequences.

**Rationale**: aitjcize is the most complete open-source implementation for this exact hardware
(modulo TG28). Waveshare demo is authoritative for panel init sequences but minimal in application
logic.

**Status**: Decided
