# aitjcize/esp32-photoframe vs our firmware — comparison

Written 2026-03-03 to document why aitjcize works on the same hardware but our
firmware cannot drive the display.  Updated 2026-03-04 to reflect all subsequent
debugging sessions.

Reference: `components/board_hal/src/driver_waveshare_photopainter_73.c`,
`components/pmic_driver_axp2101/src/axp2101.cpp`,
`components/epaper_driver_ed2208_gca/src/driver_ed2208_gca.c`,
`main/main.c` — all fetched from the aitjcize repo on 2026-03-03.

---

## Initialisation sequence in aitjcize

`board_hal_init()` does this in order:

1. I2C bus (`i2c_new_master_bus`, SDA=47, SCL=48)
2. PMIC: `axp2101_init()` + `axp2101_cmd_init()`
3. SHTC3 temperature sensor (`sensor_init`)
4. SD card (optional, `sdcard_init`)
5. SPI bus (`spi_bus_initialize`, SPI2_HOST, max_transfer_sz = 1200×825/2+100)
6. EPD: `epaper_init()` — attaches SPI device, configures GPIO, hw_reset, full init sequence

`app_main()` then inserts a **500 ms stabilisation delay** before NVS, RTC, WiFi, etc.
The first `epaper_display()` call comes much later, after WiFi connects and image
processing completes.

In our test mode, `epd_display()` is called almost immediately after `epd_init()`
(unless an explicit delay is added in the test suite setup).

---

## PMIC differences

### What `axp2101_cmd_init()` does that we don't

| Action | aitjcize | our pmic_init() |
|--------|----------|--------------------|
| Disable battery temp-pin measurement | Yes (`disableTSPinMeasure`) | No |
| Read reg 0x26, configure wakeup control | Yes (conditional) | No |
| Power key timing (4s off, 128ms on) | Yes | No |
| Charge target voltage 4.2V | Yes | No |
| VBUS current limit | **500 mA** | 1500 mA |
| Charging current | 500 mA | Not set |
| DC1 voltage = 3300 mV | Yes | No |
| ALDO3 voltage = 3300 mV | Yes (via XPowersLib) | Yes |
| ALDO4 voltage = 3300 mV | Yes (via XPowersLib) | Yes |
| System power-down (VOFF) = 2.9V | Yes | No |
| **Explicitly enable ALDO3 (EPD_VCC)** | **Never** | Yes (`pmic_epd_power(true)`) |

### Critical: aitjcize never enables ALDO3

`axp2101_cmd_init()` calls `setALDO3Voltage(3300)` but there is no `enableALDO3()`
call anywhere in the aitjcize codebase. ALDO3 is on during aitjcize testing only
because our firmware (or Waveshare firmware) left it enabled from a prior run —
the TG28 retains register state across MCU resets as long as VCC is present.

Our approach of explicitly calling `pmic_epd_power(true)` is more robust and correct.

### I2C frequency

aitjcize uses **400 kHz** for the PMIC. We use 100 kHz. No impact on EPD operation.

---

## EPD driver differences

### GPIO initialisation — ELIMINATED as root cause (2026-03-04)

**Current state of our code** (gpio_reset_pin removed, explicit level sets added):
```c
gpio_config_t out_cfg = {
    .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_CS),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,   /* matches aitjcize */
};
gpio_config(&out_cfg);
gpio_set_level(EPD_PIN_RST, 1);   /* aitjcize sets RST HIGH explicitly */
gpio_set_level(EPD_PIN_CS, 1);    /* added to match */
gpio_config_t in_cfg = {
    .mode       = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
};
gpio_config(&in_cfg);
```

Tested without any `gpio_reset_pin()` calls and with explicit CS/RST HIGH.
**Result: BUSY still stuck at second PON for 290+s. GPIO init is NOT the root cause.**

### SPI initialisation

| | aitjcize | our code |
|---|---|---|
| SPI bus | Initialised at board level before epaper_init | Inside epd_spi_init() |
| gpio_reset_pin on MOSI/CLK | **Never** | **Never** (removed) |
| SPI device attach | epaper_init → spi_bus_add_device | epd_spi_init → spi_bus_add_device |
| Speed | 10 MHz (SPI2_HOST conditional) | 10 MHz |

### Init sequence — two variants tested

**aitjcize `driver_ed2208_gca.c`** (20 commands):
```
0xAA {49 55 20 08 09 18}
0x01 {3F 00 32 2A 0E 2A}   ← 6 bytes
0x00 {5F 69}
0x03 {00 54 00 44}
0x05 {40 1F 1F 2C}
0x06 {6F 1F 16 25}          ← different
0x08 {6F 1F 1F 22}
0x13 {00 04}                ← not in Waveshare
0x30 {02}                   ← different
0x41 {00}                   ← not in Waveshare
0x50 {3F}
0x60 {02 00}
0x61 {03 20 01 E0}
0x82 {1E}                   ← not in Waveshare
0x84 {01}
0x86 {00}                   ← not in Waveshare
0xE3 {2F}
0xE0 {00}                   ← not in Waveshare
0xE6 {00}                   ← not in Waveshare
0x04                         ← POWER_ON → wait BUSY
```

**Waveshare Jan-2026 `display_bsp.cpp`** (14 commands; **currently in our code**):
```
0xAA {49 55 20 08 09 18}
0x01 {3F}                   ← 1 byte
0x00 {5F 69}
0x03 {00 54 00 44}
0x05 {40 1F 1F 2C}
0x06 {6F 1F 17 49}          ← different
0x08 {6F 1F 1F 22}
0x30 {03}                   ← different
0x50 {3F}
0x60 {02 00}
0x61 {03 20 01 E0}
0x84 {01}
0xE3 {2F}
0x04                         ← POWER_ON → wait BUSY
```

Both sequences were tested. **Neither fixed the BUSY-stuck issue.** The init sequence
is not the root cause of the display failure.

### Refresh sequence — current state

```
0x10                          ← DTM (start of pixel data)
[192,000 bytes, 4000-byte chunks, CS held LOW throughout]
[vTaskDelay(1) after EVERY chunk]
0x04                          ← PON → wait BUSY
0x12 {00}                     ← DRF → wait BUSY (~30 s)
0x02 {00}                     ← POF → wait BUSY
```

---

## Full test matrix (all results on our hardware)

All tests were done on USB power, no battery, with aitjcize known-working on the same setup.

| # | Init | Transfer | Extra delay | Sequence | Result |
|---|------|----------|-------------|----------|--------|
| 1 | aitjcize | Fast 5000B, every 10 chunks | 0 s | DTM→PON→DRF→POF | **BUSY stuck at PON 290+s** |
| 2 | aitjcize | Fast 5000B, every 10 chunks | 2 s | DTM→PON→DRF→POF | PON OK, DRF **instant** (no waveform) |
| 3 | aitjcize | Fast 5000B, no PON in init | 0 s | DTM→PON→DRF→POF | **BUSY stuck at PON 400s** (timeout) |
| 4 | aitjcize | Fast 5000B, every 10 chunks | 0 s | POF+DTM→PON→DRF→POF | DRF **instant** |
| 5 | aitjcize | Fast 5000B, every 10 chunks | 0 s | POF+DTM→PON→0x06→DRF→POF | DRF **instant** |
| 6 | aitjcize | Fast 5000B, every 10 chunks | 0 s | DTM→DRF (no PON)→POF | **BUSY stuck at DRF 300+s** |
| 7 | aitjcize | Fast 5000B, every 10 chunks | 0 s | DTM→PON→0x06→DRF→POF | **BUSY stuck at PON 300+s** |
| 8 | Waveshare | Fast 5000B, every 10 chunks | 0 s | DTM→PON→DRF→POF | **BUSY stuck at PON 290+s** |
| 9 | Waveshare | Fast 5000B (no gpio_reset) | 0 s | DTM→PON→DRF→POF | **BUSY stuck at PON 290+s** |
| 10 | Waveshare | Fast 5000B, every 10 chunks | **60 s** | DTM→PON→DRF→POF | All commands **instant** (no waveform) |
| 11 | Waveshare | Fast 5000B, every 10 chunks | 0 s | DTM→0x06→DRF (no PON)→POF | **BUSY stuck at DRF 80+s** |
| 12 | Waveshare | **Slow 4000B, every chunk** | 0 s | DTM→PON→DRF→POF | All commands **instant** (no waveform) |

---

## Key finding: Fast vs slow SPI transfer dichotomy

This is the most important finding from the debugging sessions. The transfer speed
alone completely changes the failure mode:

**Fast transfer** (5000-byte chunks, vTaskDelay(1) every 10 chunks = ~100 ms between yields):
- The panel **starts a waveform** — BUSY goes and stays LOW for 290-400 seconds
- The waveform never terminates (BUSY never returns HIGH)
- This happens regardless of init sequence (aitjcize or Waveshare)

**Slow transfer** (4000-byte chunks, vTaskDelay(1) after EVERY chunk = ~10 ms between chunks):
- The panel accepts all commands but **runs no waveform** — BUSY returns HIGH instantly
- Same instant-completion behavior as when a 60 s delay is inserted after init
- The display does not update visually

**Why this matters**: aitjcize uses 4000-byte chunks with vTaskDelay(1) after EVERY chunk —
the exact slow-transfer pattern — and it WORKS (30 s waveform, display updates).
Our identical slow-transfer code produces instant BUSY HIGH with no waveform.

The slow-transfer pattern appears to put our panel into an auto-sleep state before
(or during) the PON command, so PON is acknowledged but triggers no waveform. The
fast-transfer pattern successfully starts a waveform but the waveform never terminates.

**Working hypothesis**: There is a state we're not reaching that aitjcize does reach
between init and first display. Something in aitjcize's longer init→display gap (or
perhaps a subtle sequencing detail we haven't identified) correctly primes the panel
so that slow-transfer + PON launches a 30 s waveform. We have not found what that
state is.

---

## Eliminated hypotheses

### ~~Hypothesis: Power under load~~  — ELIMINATED (2026-03-03)

The user confirmed aitjcize was tested on the **same USB-only, no-battery setup**.
VBUS current limits, battery supplement, etc. are not the cause.

### ~~Hypothesis: gpio_reset_pin() interference~~  — ELIMINATED (2026-03-04)

Removed all `gpio_reset_pin()` calls from `epd_gpio_init()` and `epd_spi_init()`.
Added explicit `gpio_set_level(RST, 1)` and `gpio_set_level(CS, 1)` after
`gpio_config()`. Added `pull_up_en` on output pins to match aitjcize exactly.
**Result: no change in behavior.** GPIO init is not the root cause.

### ~~Hypothesis: Init sequence bytes~~  — ELIMINATED (2026-03-04)

Switched from aitjcize bytes to Waveshare bytes (different 0x01, 0x06, 0x30; 6
fewer commands). Identical BUSY-stuck-at-PON result with fast transfer. The
specific init bytes are not the cause.

### ~~Hypothesis: Panel settling time after init PON~~  — PARTIALLY CONFIRMED, but more complex

A 60 s delay between `epd_init()` and `epd_display()` changes the failure mode
from "stuck at PON 290+s" to "instant BUSY HIGH (no waveform)". So timing IS
relevant — but the 60 s delay overcorrects. The panel auto-sleeps after the init
PON within 30-60 s without receiving further commands, and then the subsequent
slow-transfer DTM + PON is not enough to wake it into a refresh waveform.

---

## Remaining unknown

The core mystery: **aitjcize uses the identical slow-transfer pattern on the same
hardware and gets a working 30 s refresh. We don't.**

Things that differ between aitjcize and us at the point `epaper_display()` is called:

1. **Time elapsed since init PON**: aitjcize = 10-60 s (WiFi + image processing);
   our slow-transfer test = ~0 s. A 2 s delay (test 2) makes PON instant, not waveform.
   A 60 s delay (test 10) makes all commands instant. The working window in aitjcize
   is narrow and we haven't found it.

2. **Second PON in the refresh sequence**: aitjcize sends PON (0x04) AFTER the DTM
   data. We do too (current code). But aitjcize's panel is still in an active PON
   state from init when DTM arrives (init PON never timed out because image processing
   happened quickly enough). The second PON in aitjcize is therefore PON→PON
   (re-trigger), whereas in our slow-transfer test the panel may have auto-POF'd.

3. **SPI state between init and display**: aitjcize initialises the SPI bus at board
   level and holds it throughout. Our code holds it too (epd_spi_init persists), but
   the panel's SPI receive state machine may behave differently depending on whether
   it has been in PON or auto-POF'd state when the DTM arrives.

---

## What to try next (in priority order)

1. **Scope the BUSY pin** during a refresh attempt with a logic analyser or oscilloscope.
   Observe whether BUSY ever pulses HIGH briefly then drops LOW (panel starts then
   aborts), or stays continuously LOW (never starts). This would distinguish between
   "waveform starts but stalls" and "panel ignores DTM entirely".

2. **Test with an intermediate delay** (3-10 s) between `epd_init()` and `epd_display()`.
   The 2 s delay gave instant DRF (test 2); 60 s gave instant all commands (test 10).
   There may be a narrow window (say 5-20 s) where the panel is in the right state
   for a waveform to start AND complete. This would confirm whether it's purely a
   timing issue.

3. **Keep the panel in PON state continuously** — send a dummy DTM immediately after
   the init PON (fill with 0x11 = white/white) and use DRF to trigger the init refresh,
   then immediately send the real frame + DRF. This ensures PON is never idle long
   enough to auto-sleep.

4. **Compare SPI signal timing with a scope** — put aitjcize and our firmware on a
   scope simultaneously (or alternately on the same setup) and compare the actual SPI
   waveforms during the 192 KB transfer. A difference in CS idle state, DC timing,
   or inter-chunk spacing might show what the panel is responding to differently.
