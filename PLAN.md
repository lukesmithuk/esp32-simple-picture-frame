# Project Plan

Battery-powered e-ink picture frame on the Waveshare ESP32-S3-PhotoPainter. Wakes once per day,
updates the displayed image from SD card (or WiFi), then returns to deep sleep. Target: months of
battery life per charge.

## Phases

### Phase 1: Hardware Bring-Up
Verify all peripherals are reachable and behaving as expected before writing application logic.

- [ ] I2C scan — confirm addresses for TG28 PMIC (expected 0x34), PCF85063 RTC (0x51), SHTC3 (0x70)
- [ ] TG28 chip ID — read register 0x03; AXP2101 returns 0x47; document actual value
- [ ] EPD power-on — assert GPIO 6 high, verify SPI comms with display controller
- [ ] SD card mount — FAT32, read a test file
- [ ] RTC read — confirm time registers are accessible

### Phase 2: EPD Driver
Minimal driver to push a full-frame image to the 7.3" Spectra 6 panel.

- [ ] SPI initialisation (MOSI=11, CLK=10, CS=9, DC=8, RST=12, BUSY=13, PWR=6)
- [ ] Panel init sequence (from Waveshare demo / aitjcize reference)
- [ ] Full-frame write (800×480, 4bpp packed, ~192 KB framebuffer in PSRAM)
- [ ] Busy-wait on BUSY pin (~30 s refresh)
- [ ] Panel sleep command after refresh

### Phase 3: Image Pipeline
Convert arbitrary images to the Spectra 6 6-colour palette for display.

- [ ] Define measured palette (RGB values for Black/White/Green/Blue/Red/Yellow as rendered)
- [ ] Floyd-Steinberg dithering over 800×480 framebuffer
- [ ] JPEG decode from SD card (esp_jpeg or libjpeg-turbo component)
- [ ] Resize/crop to 800×480
- [ ] End-to-end: JPEG on SD → dithered framebuffer → EPD

### Phase 4: Wake / Sleep Cycle
- [ ] Deep sleep entry with RTC alarm wakeup (PCF85063)
- [ ] Persist image index across sleep (RTC RAM or NVS)
- [ ] First-boot detection
- [ ] Manual trigger via button (optional)

### Phase 5: WiFi Image Fetch (optional / later)
- [ ] WiFi STA connect
- [ ] HTTP GET image from configurable URL
- [ ] Fallback to SD card on failure

### Phase 6: Power Optimisation
- [ ] Measure current draw during each phase
- [ ] Profile wake cycle wall-clock time
- [ ] Tune sleep voltage / peripherals-off sequence via TG28
- [ ] Estimate battery life (target: 3+ months on a 2000 mAh cell)

## Milestones

| # | Milestone | Status |
|---|-----------|--------|
| 1 | All peripherals confirmed reachable via I2C scan | pending |
| 2 | Static test image displayed on EPD | pending |
| 3 | SD-card JPEG rendered on EPD end-to-end | pending |
| 4 | Device wakes, updates, sleeps reliably 10× in a row | pending |
| 5 | Battery life estimate validated against target | pending |
