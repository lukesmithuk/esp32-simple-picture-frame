# TODO

## Completed Phases

- [x] Phase 1: Hardware bring-up & schematic analysis
- [x] Phase 2: PMIC driver (AXP2101 via XPowersLib, bit-banged I2C)
- [x] Phase 3: EPD driver — first attempt (abandoned, BUSY-stuck bug)
- [x] Phase 4: Architecture definition + fresh EPD implementation (verified on hardware)
- [x] Phase 5: RTC alarm + deep sleep wake cycle
- [x] Phase 6: SD card mount, image picker, loader, error display, error logging
- [x] Phase 7: JPEG decode → scale → CDR → Floyd-Steinberg dither → display
- [x] Phase 9: Production loop — log to file, image shuffle, configurable wake, full cycle verified

## Phase 8 — Image Pipeline Enhancements

- [ ] EXIF orientation — TJpgDec ignores EXIF tags; phone photos may display rotated
- [ ] Portrait auto-rotation — detect portrait aspect ratio, rotate to fill landscape display
- [ ] Progressive JPEG support — TJpgDec (ROM) only handles baseline; would need a different decoder (e.g. libjpeg-turbo)
- [ ] PNG support — libpng streaming decode
- [ ] Palette calibration — measure this specific panel's colours for more accurate dithering
- [ ] S-curve tone mapping — perceptual tone curve before dither (aitjcize reference has this)

## Phase 9 — Production Loop & Reliability

- [x] Full cycle verified: alarm wake → image select → decode → dither → display → sleep
- [x] Log to file — applog component: buffer early boot to RAM, tee ESP_LOG to SD card file
- [x] Configurable wake interval — config component: key=value file from SD card
- [x] Image shuffle without repeat — history file in image directory, cycle without repeats

## Phase 10 — Power Optimisation

- [ ] Map DLDO1/DLDO2 rails — determine what they power (USB-JTAG serial bridge?)
- [ ] Enable `board_sleep()` in production — currently disabled because DLDO1/DLDO2 cut kills serial
- [ ] Measure deep-sleep current consumption
- [ ] Battery support — read battery level from AXP2101, low-battery warning on display

## Security

- [ ] HTTPS transport — TLS on ESP32-S3 (`esp_tls`) + certificate on Pi; would protect API key in transit
- [ ] Session-based web UI auth — replace injected API key with login/session cookies for web UI actions
- [ ] API key not visible in page source — currently injected into HTML template for JS delete handler

## Build / Tooling

- [x] FATFS LFN — fixed by deleting build/ and sdkconfig to regenerate from sdkconfig.defaults
