# TODO

Granular task list. Items are grouped by phase and ordered by priority within each group.
Move completed items to PROGRESS.md.

## Immediate / Unblocked

- [ ] Scaffold ESP-IDF project structure (`idf.py create-project` or manual CMakeLists)
- [ ] Set target: `idf.py set-target esp32s3`
- [ ] Verify toolchain builds a hello-world against the board
- [ ] Write minimal I2C scan utility (scan 0x00–0x7F, log addresses found)

## Phase 1 — Hardware Bring-Up

- [ ] Flash I2C scan, record which addresses respond
- [ ] Read TG28 register 0x03 — document returned chip ID
- [ ] If TG28 ≠ AXP2101 compatible: research TG28 register map, open issue
- [ ] Confirm PCF85063 RTC responds at 0x51
- [ ] Confirm SHTC3 responds at 0x70
- [ ] Verify GPIO 6 (EPD PWR) high → EPD powers on (check BUSY pin behaviour)
- [ ] Mount SD card via `esp_vfs_fat`, list root directory
- [ ] Pull schematic from Waveshare wiki — verify PCF85063 INTB wiring to ESP32-S3 GPIO

## Phase 2 — EPD Driver

- [ ] Port / adapt EPD init sequence from Waveshare demo
- [ ] Implement SPI send for full-frame buffer
- [ ] Test: display a solid-colour frame (all-black, all-white)
- [ ] Test: display a hardcoded 2-colour checkerboard
- [ ] Implement panel sleep command
- [ ] Measure EPD refresh time with stopwatch (baseline)

## Phase 3 — Image Pipeline

- [ ] Obtain measured Spectra 6 palette RGB values (from aitjcize repo or self-measured)
- [ ] Implement Floyd-Steinberg dithering in C
- [ ] Write unit test for dithering (feed synthetic gradient, check output palette indices)
- [ ] Integrate JPEG decoder component (evaluate `esp_jpeg` vs libjpeg-turbo)
- [ ] Implement resize/crop to 800×480 (nearest-neighbour first, lanczos later if needed)
- [ ] End-to-end test: put a JPEG on SD, render it on EPD

## Phase 4 — Wake / Sleep

- [ ] Implement PCF85063 alarm set/clear
- [ ] Implement deep sleep entry (esp_deep_sleep_start)
- [ ] Test: device wakes from RTC alarm, logs timestamp, sleeps again
- [ ] Persist image index in RTC fast memory (survives deep sleep)
- [ ] Handle first-boot (RTC memory uninitialised)

## Phase 5 — WiFi (deferred)

- [ ] WiFi STA connect with configurable SSID/password (NVS)
- [ ] HTTP GET raw image URL
- [ ] Fallback to SD on HTTP failure

## Phase 6 — Power / Battery Life

- [ ] Measure sleep current (target: <100 µA)
- [ ] Measure peak wake current and wake duration
- [ ] Calculate estimated daily mAh consumption
- [ ] Project battery life against known cell capacity
- [ ] Tune PMIC output voltages if TG28 allows it

## Backlog / Nice-to-Have

- [ ] OTA firmware update via WiFi
- [ ] Simple web UI for image upload to SD
- [ ] Temperature/humidity display overlay (SHTC3)
- [ ] Low-battery indicator on EPD
