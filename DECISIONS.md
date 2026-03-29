# Architecture Decision Records

## ADR-001 — Port aitjcize drivers directly

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** Port drivers from `aitjcize/esp32-photoframe` directly rather than
rewriting from scratch.

**Rationale:** aitjcize's firmware is confirmed working on this exact hardware.
Minimising delta from a known-good baseline reduces debugging surface.

---

## ADR-002 — Use XPowersLib (C++) for AXP2101 PMIC

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** Vendor XPowersLib in-tree inside `components/board/XPowersLib/`
and use it via `axp2101.cpp` (C++ wrapper with `extern "C"` public API), exactly
as aitjcize does.

**Rationale:** XPowersLib provides a complete, maintained AXP2101 register
abstraction. Writing a pure-C driver would require duplicating all register-level
knowledge. The `XPOWERS_CHIP_AXP2101` + `CONFIG_XPOWERS_ESP_IDF_NEW_API` compile
defines make it work with our hardware's chip ID (0x4A).

**Tradeoff:** Adds a C++ dependency. ESP-IDF handles mixed C/C++ components fine.

---

## ADR-003 — Board HAL component owns I2C bus

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** `components/board/` owns the I2C master bus handle and wraps all
I2C devices (AXP2101 PMIC, PCF85063 RTC). EPD is a separate SPI-only component.
Main never touches I2C directly.

**Rationale:** PMIC and RTC share the same I2C bus. The board init sequence has
hardware-mandated ordering (I2C → PMIC → ALDO3 enable → EPD). Centralising this
in a board component keeps `main.c` clean and makes the ordering explicit.

---

## ADR-004 — EPD init + re-init on every display call

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** `epd_display()` performs a full `hw_reset + init_sequence + refresh`
cycle on every call. The panel is not kept powered between refreshes.

**Rationale:** This matches aitjcize's confirmed-working pattern. The previous
attempt (init once at startup, refresh separately) produced a BUSY-stuck bug.

---

## ADR-005 — wait_busy after data transfer, before PON

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** The EPD refresh sequence inserts `wait_busy("data")` between the
`0x10` data transfer and the `0x04 PON` command.

**Rationale:** This is the most likely root cause of the BUSY-stuck bug in the
previous attempt. aitjcize's working driver includes this wait; our previous
firmware did not.

---

## ADR-006 — 128-byte CS windows, 20 MHz SPI

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** EPD frame buffer sent in 128-byte CS windows (each chunk copied to
a stack-local buffer). SPI clock 20 MHz. No inter-chunk `vTaskDelay`.

**Rationale:** Matches aitjcize's working driver. Previous attempt used 5000-byte
chunks at 10 MHz with inter-chunk delays — divergence from the reference without
verified benefit.

---

## ADR-007 — I2C bus recovery on every boot

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** `board_init()` manually toggles SCL 9 times + sends a STOP condition
before calling `i2c_new_master_bus()`.

**Rationale:** Ported from aitjcize's `board_hal_init()`. Handles the case where
deep sleep or a crash interrupted a previous I2C transaction, leaving a device
holding SDA low. Without this, the I2C bus can be stuck at boot.

---

## ADR-008 — board_sleep() not called in debug builds

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** `board_sleep()` (→ `axp2101_basic_sleep_start()`) is stubbed out in
the production path for now. It will be enabled in Phase 10 only after the DLDO1/
DLDO2 rail is mapped.

**Rationale:** DLDO1/DLDO2 power something the USB-JTAG serial bridge depends on.
Calling `board_sleep()` silences serial output, making debug impossible. Must map
the rails before enabling.

---

## ADR-009 — Test infrastructure: Unity + custom TEST_MODE

**Status:** Accepted
**Date:** 2026-03-08

**Decision:** Two complementary test mechanisms:
1. **Unity component tests** (`components/*/test/`, `idf.py -T`): pure logic,
   no hardware required. Currently covers EPD pixel-packing.
2. **Hardware integration tests** (`TEST_MODE` kconfig, `main/test_main.c`):
   hardware-in-the-loop. Covers PMIC power toggle, RTC availability, EPD
   solid-colour display for all 6 palette colours.

**Rationale:** Unity is the ESP-IDF standard and enables host-side testability
for pure logic (e.g. dithering algorithm, image scaling). Hardware-in-the-loop
tests require the physical board and are best driven by the existing serial
capture workflow.

---

## ADR-010 — Spectra 6 panel colour indices

**Status:** Accepted
**Date:** 2026-03-14

**Decision:** `epd_color_t` uses the actual panel hardware indices:
0=Black, 1=White, 2=Yellow, 3=Red, 5=Blue, 6=Green. Index 4 is reserved
(clean). Orange does not exist on this panel.

**Rationale:** The panel is E Ink Spectra 6 (E6) with 6 physical pigments.
Confirmed from aitjcize's `epaper.h` defines and Waveshare product page.
The previous enum (Green=2, Blue=3, Red=4, Yellow=5, Orange=6) was wrong
and caused incorrect colour output during dithering.

---

## ADR-011 — 180° frame buffer rotation in epd_display

**Status:** Accepted
**Date:** 2026-03-14

**Decision:** `epd_display()` rotates the frame buffer 180° (reverse byte
order + swap nibbles) into a temporary PSRAM copy before SPI transfer.

**Rationale:** The panel's scan origin is bottom-right, but our logical
coordinate system (and text renderer) assumes top-left origin. Rotating
at the driver level fixes all content (images, text, error messages)
without requiring each component to handle orientation. The 192 KB temp
buffer is freed immediately after transfer.

---

## ADR-012 — Measured palette for Floyd-Steinberg dithering

**Status:** Accepted
**Date:** 2026-03-14

**Decision:** Dithering uses measured RGB values from the physical panel
rather than theoretical sRGB values. Source: aitjcize/esp32-photoframe
calibration data.

**Rationale:** E-paper displays show colours 30–70% darker than theoretical
values (e.g. white is actually (190,200,200) not (255,255,255)). Using
measured values for colour distance calculations and error diffusion
produces significantly more accurate dithering output.

---

## ADR-013 — Dynamic range compression before dithering

**Status:** Accepted
**Date:** 2026-03-14

**Decision:** Apply compress-dynamic-range (CDR) pass between scaling and
dithering. Uses sRGB↔linear LUTs and BT.709 luminance to map the image's
tonal range into [measured_black_Y, measured_white_Y].

**Rationale:** The e-paper panel has ~90:1 dynamic range vs ~1000:1+ for
typical displays. Without CDR, highlights crush to the same grey and
shadows crush to the same near-black. CDR redistributes tones to use
the panel's available range, preserving detail in highlights and shadows.

---

## ADR-014 — WiFi-first image fetch with SD card fallback

**Status:** Accepted
**Date:** 2026-03-16

**Decision:** When `wifi_ssid` is configured, the frame connects to WiFi
and fetches the next image from a self-hosted FastAPI server. If WiFi
connect fails, the server is unreachable, or the gallery is empty, the
frame falls back to the existing SD card image picker.

**Rationale:** WiFi enables remote photo management via web UI without
physically accessing the SD card. SD card fallback ensures the frame
always displays something, even without network.

---

## ADR-015 — Server-side image processing on upload

**Status:** Accepted
**Date:** 2026-03-16

**Decision:** The server converts all uploaded images to baseline JPEG,
resizes to 800×480 (cover mode, Lanczos), and strips alpha on upload.
Accepts JPEG, PNG, and WebP input formats.

**Rationale:** Processing on upload means the frame always receives an
optimally-sized baseline JPEG, eliminating progressive JPEG decode
failures and reducing download size and decode time. The Pi has more
CPU/memory than the ESP32 for image processing.

---

## ADR-016 — PMIC sleep before deep sleep

**Status:** Accepted
**Date:** 2026-03-17

**Decision:** Call `board_sleep()` (AXP2101 `enableSleep()` + disable
all rails) immediately before `esp_deep_sleep_start()`.

**Rationale:** Reduces deep sleep current by putting the PMIC into
low-power mode. DLDO1/DLDO2 pins confirmed unconnected on the Waveshare
PhotoPainter schematic — the earlier concern about them powering the
USB-JTAG bridge was unfounded. Serial output stops after `board_sleep()`
but the ESP32 is about to enter deep sleep anyway.

---

## ADR-017 — Server image sync on startup

**Status:** Accepted
**Date:** 2026-03-17

**Decision:** On server startup, scan the `images/` directory and add
any files not already in the SQLite database.

**Rationale:** Allows the database to be safely deleted (e.g. for schema
changes) without losing uploaded images. The image files persist on disk
and are re-registered automatically on next startup.

---

## ADR-018 — Server-rendered web UI without build tools

**Status:** Accepted
**Date:** 2026-03-29

**Decision:** The server web UI uses Jinja2 templates with vanilla CSS and
JavaScript. No frontend framework (React, Vue), no build tools (Vite,
webpack), no Node.js dependency.

**Rationale:** The server's deployment target is a Raspberry Pi Zero 2W
(512MB RAM). Adding Node.js for a build step would increase the install
footprint and complexity for what is essentially a settings/upload page.
Server-rendered templates keep the tarball self-contained — extract and
run. System fonts are used instead of web fonts to avoid external CDN
requests on networks without internet access.
