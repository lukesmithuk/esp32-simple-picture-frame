# TODO

## Completed

- [x] Phase 1: Hardware bring-up & schematic analysis
- [x] Phase 2: PMIC driver (AXP2101 via XPowersLib, bit-banged I2C)
- [x] Phase 3: EPD driver — first attempt (abandoned, BUSY-stuck bug)
- [x] Phase 4: Architecture definition + fresh EPD implementation (verified on hardware)
- [x] Phase 5: RTC alarm + deep sleep wake cycle
- [x] Phase 6: SD card mount, image picker, loader, error display, error logging
- [x] Phase 7: JPEG decode → scale → CDR → Floyd-Steinberg dither → display
- [x] Phase 9: Production loop — log to file, image shuffle, configurable wake, full cycle verified
- [x] WiFi photo retrieval — server + firmware, NTP sync, status/log push, web UI
- [x] Phase 10: Power optimisation — PMIC sleep, battery status, DLDO mapping
- [x] Build / Tooling — FATFS LFN, server install package, pinned deps, web UI redesign
- [x] Phase 11: Multi-frame — frame naming, per-frame galleries, per-frame wake interval

## Phase 8 — Image Quality & Server Processing

**Server-side (upload processing):**
- [ ] EXIF orientation — apply `ImageOps.exif_transpose` during upload so phone photos display correctly
- [ ] Portrait auto-rotation — rotate portrait images to landscape during upload
- [x] Progressive JPEG — converted to baseline on upload
- [x] PNG/WebP — converted to JPEG on upload

**Frame-side (dither quality):**
- [ ] Palette calibration — measure this specific panel's colours for more accurate dithering
- [ ] S-curve tone mapping — perceptual tone curve before dither (aitjcize reference has this)

## Phase 10 — Power Optimisation

- [x] Map DLDO1/DLDO2 rails — confirmed unconnected on schematic
- [x] Enable `board_sleep()` in production — PMIC low-power mode before deep sleep
- [ ] Measure deep-sleep current consumption
- [x] Battery status — ADC enabled, voltage/percent/charge logged and pushed to server
- [ ] Low-battery warning — display message on EPD when battery critically low

## Security

- [ ] HTTPS transport — TLS on ESP32-S3 (`esp_tls`) + certificate on Pi; would protect API key in transit
- [ ] Session-based web UI auth — replace injected API key with login/session cookies for web UI actions
- [ ] API key not visible in page source — removed from inline JS (now uses data attributes), but still in hidden form field for upload
- [ ] Limit log upload body size — `/api/logs` reads full request without size cap; add 64KB limit or use uvicorn `--limit-request-body`
- [ ] Reject default API key — refuse to start or warn loudly if key is "changeme"
- [ ] Remove API key from database — stored unnecessarily in frames table; remove column or hash it
- [ ] Constant-time key comparison — use `hmac.compare_digest()` instead of `!=` for API key checks
- [ ] Sanitize upload filenames — strip special characters, limit length

## Web UI

- [ ] Live updates — auto-refresh frames table and gallery (polling or SSE) so the dashboard stays current without manual reload
