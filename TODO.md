# TODO

## Immediate (Phase 4 — current)

- [ ] Build and verify the fresh implementation compiles cleanly
- [ ] Flash and run `TEST_MODE` to verify:
  - PMIC init (chip ID 0x4A, no error)
  - PMIC EPD power toggle (ALDO3 on/off)
  - RTC init (PCF85063 detected)
  - EPD solid white/black display (verify BUSY-stuck bug is fixed)
  - EPD all 6 colours

## Phase 5 — RTC alarm + deep sleep wake

- [ ] Implement PCF85063 alarm registers (0x0B–0x0F, AEN bits)
- [ ] Implement alarm flag clear (Control_2 register 0x01, AF bit)
- [ ] Configure ESP32 EXT0 wakeup on GPIO6 (active-LOW from PCF85063)
- [ ] Wire `board_sleep()` + `esp_deep_sleep_start()` in production path
- [ ] Test full sleep/wake cycle

## Phase 6 — SD card

- [ ] Mount SD card (4-bit SDIO: D3/CS=38, CLK=39, D0=40, CMD=41, D1=1, D2=2)
- [ ] List/select images from `/sdcard/images/`
- [ ] Persist current image index (NVS or file)

## Phase 7 — Image decode

- [ ] JPEG decode: `esp_jpeg_decode_one_picture()`
- [ ] PNG decode: libpng streaming (128 rows, PSRAM alloc)
- [ ] Bilinear scale to 800×480

## Phase 8 — Dithering

- [ ] Floyd-Steinberg dither to 6-colour palette
- [ ] Use **measured** palette values (not theoretical sRGB)
  - TODO: locate measured palette values in aitjcize repo

## Phase 9 — Production loop

- [ ] Full cycle: alarm wake → image select → decode → dither → display → sleep
- [ ] Error handling: fallback to test pattern if SD/image fails

## Phase 10 — Power optimisation

- [ ] Map DLDO1/DLDO2 rails (what do they power?)
- [ ] Enable `board_sleep()` in production once rails are safe to cut
- [ ] Measure deep-sleep current consumption
