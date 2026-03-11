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
