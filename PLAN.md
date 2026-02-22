# Project Plan

Battery-powered e-ink picture frame on the Waveshare ESP32-S3-PhotoPainter. Wakes once per day,
updates the displayed image from SD card (or WiFi), then returns to deep sleep. Target: months of
battery life per charge.

## Boot-cycle model (ADR-012)

This firmware has **no main loop**.  Each display update is a complete cold-boot → work → sleep
cycle.  `esp_deep_sleep_start()` never returns; the PCF85063 RTC alarm on GPIO6 (EXT0, active LOW)
triggers a cold boot and `app_main()` runs again from the top.

```
boot → app_main() → init → update → pmic_sleep() → deep_sleep_start()
          ↑                                                  |
          └──────── PCF85063 alarm → GPIO6 → cold boot ─────┘
```

Persistent state (image index, last-display time) lives in RTC fast memory or NVS.

## Phases

### Phase 1: Hardware Bring-Up ✓ COMPLETE (2026-02-21)
Verified all peripherals reachable. Key findings: TG28 = AXP2101 register-compatible
(chip ID 0x4A), EPD power via TG28 ALDO3 (no GPIO), SD card is 4-bit SDIO.

### Phase 2: PMIC Driver ✓ COMPLETE (2026-02-22)
Pure-C TG28 driver (`components/pmic/`). EPD power (ALDO3), sleep rails, production
boot cycle verified on hardware.

### Phase 3: EPD Driver ✓ COMPLETE (2026-02-22)
Pure-C SPI driver (`components/epd/`) for the 7.3" Spectra 6 panel. SPI2 at 40 MHz
half-duplex. Init sequence, 5000-byte chunked frame transfer, refresh, and sleep
sequences implemented and build-verified. Hardware visual test still pending.

### Phase 4: Image Pipeline
Convert arbitrary images to the Spectra 6 6-colour palette for display.

- [ ] Find measured Spectra 6 palette in aitjcize epaper component
- [ ] Floyd-Steinberg dithering in C against measured palette (PSRAM working buffer)
- [ ] JPEG decode: `esp_jpeg_decode_one_picture()` → RGB888 in PSRAM
- [ ] Bilinear scale (fixed-point ×1024) to fit 800×480
- [ ] Define portrait image policy (letterbox or crop-to-fill)
- [ ] End-to-end: JPEG on SD → decode → scale → dither → EPD

### Phase 5: Wake / Sleep Cycle
- [ ] Port aitjcize pcf85063.c (time read/write, OSF check)
- [ ] Implement PCF85063 alarm registers (0x0B–0x0F, AEN bits)
- [ ] Remove debug halt; call enter_deep_sleep() (blocked on Phase 7 DLDO rail mapping)
- [ ] Persist image index in RTC fast memory
- [ ] Handle first-boot detection

### Phase 6: WiFi Image Fetch (deferred)
- [ ] WiFi STA connect with configurable SSID/password (NVS)
- [ ] HTTP GET raw image URL
- [ ] Fallback to SD on HTTP failure

### Phase 7: Power Optimisation
- [ ] Map LDO_EN_3 DLDO1/DLDO2 rail assignments (blocks pmic_sleep() in production)
- [ ] Map DCDC_EN bit→rail for TG28
- [ ] Measure sleep current (target < 100 µA)
- [ ] Measure wake cycle duration and peak current
- [ ] Project battery life against 2000 mAh target

## Milestones

| # | Milestone | Status |
|---|-----------|--------|
| 1 | All peripherals confirmed reachable via I2C scan | **DONE** (2026-02-21) |
| 2 | PMIC driver: EPD power + sleep sequence working | **DONE** (2026-02-22) |
| 2a | Production boot verified on hardware (pmic_init + EPD power cycle) | **DONE** (2026-02-22) |
| 3 | EPD driver implemented; build clean | **DONE** (2026-02-22) |
| 3a | Solid colour fills verified visually on hardware | pending |
| 4 | SD-card JPEG rendered on EPD end-to-end | pending |
| 5 | Device wakes, updates, sleeps reliably 10× in a row | pending |
| 6 | Battery life estimate validated against target | pending |
