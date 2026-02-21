# Test Plan

Verification strategy for each subsystem. Tests are ordered from hardware primitives up to full
system integration. Each test has a pass criterion and a failure action.

---

## T1 — Toolchain & Board Connectivity

**Goal**: Confirm the development environment can build and flash firmware.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T1.1 | `idf.py build` on hello-world | Zero errors, binary produced |
| T1.2 | `idf.py flash` via USB-C | Flash completes without error |
| T1.3 | `idf.py monitor` | Serial output visible at 115200 baud |

**Failure action**: Check USB cable (data-capable), driver (`/dev/ttyUSB0` visible), dialout group
membership (`groups $USER`), and download mode (BOOT + PWR).

---

## T2 — I2C Bus Scan

**Goal**: Confirm all expected I2C devices are reachable.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T2.1 | Flash I2C scanner (SDA=47, SCL=48) | At least 3 addresses respond |
| T2.2 | Check 0x34 | TG28 PMIC present |
| T2.3 | Check 0x51 | PCF85063 RTC present |
| T2.4 | Check 0x70 | SHTC3 sensor present |

**Failure action**: If an address is missing, check pull-up resistors (should be on-board),
verify SDA/SCL pin mapping in schematic, try lower I2C frequency (100 kHz).

---

## T3 — TG28 PMIC Identification

**Goal**: Determine whether TG28 is register-compatible with AXP2101.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T3.1 | Read register 0x03 at I2C addr 0x34 | Returns a value without NAK |
| T3.2 | Compare value to 0x47 (AXP2101 chip ID) | Document result either way |

**Outcomes**:
- Returns 0x47 → TG28 is likely AXP2101-compatible; can adapt aitjcize driver directly.
- Returns other value → TG28 has different register map; need TG28 datasheet before using PMIC features.
- NAK → wrong address or PMIC not present; re-scan I2C bus.

---

## T4 — EPD Power & SPI Connectivity

**Goal**: Confirm the e-paper display powers on and SPI is functional.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T4.1 | Drive GPIO 6 high | No crash; BUSY pin changes state within 1 s |
| T4.2 | Send EPD init sequence via SPI | No SPI timeout errors in log |
| T4.3 | Display full-black frame | Panel refreshes (~30 s), uniform black result |
| T4.4 | Display full-white frame | Panel refreshes, uniform white result |
| T4.5 | Send panel sleep command | BUSY pin goes low, no high-voltage damage over 1 hour |

**Failure action**: Verify SPI pin mapping (MOSI=11, CLK=10, CS=9, DC=8, RST=12, BUSY=13).
Check GPIO 6 drive strength. Compare init sequence byte-for-byte against Waveshare demo.

---

## T5 — SD Card

**Goal**: Mount SD card and read/write files.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T5.1 | Mount FAT32 SD via `esp_vfs_fat` | Mount succeeds, no error log |
| T5.2 | List root directory | File listing matches card contents |
| T5.3 | Read a known test file | Bytes match expected content |
| T5.4 | Write a small file | File readable after unmount/remount |

**Failure action**: Confirm card is FAT32 (not exFAT). Try lower SPI speed (4 MHz). Check
SD card slot connections (uses same SPI bus as EPD or separate — confirm from schematic).

---

## T6 — Image Dithering (Unit-Level)

**Goal**: Validate Floyd-Steinberg dithering produces correct palette indices.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T6.1 | Feed solid-black input | All output pixels index = Black |
| T6.2 | Feed solid-white input | All output pixels index = White |
| T6.3 | Feed 50% grey gradient | Output is mix of Black+White pixels, no out-of-range indices |
| T6.4 | Feed primary colour patches | Dominant palette index matches expected colour |
| T6.5 | Visual inspection on EPD | Dithered image looks correct at arm's length |

**Note**: T6.1–T6.4 can be run on host (Linux) without hardware, using a test harness that
writes output as a PPM image for visual inspection.

---

## T7 — JPEG Decode & Resize

**Goal**: Confirm JPEG files can be decoded and scaled to 800×480.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T7.1 | Decode a 800×480 JPEG from SD | No decoder errors, pixel buffer populated |
| T7.2 | Decode a larger JPEG (e.g. 1920×1080), resize to 800×480 | Output dimensions correct |
| T7.3 | Decode a portrait JPEG (e.g. 480×800) | Handled gracefully (crop or letterbox — define policy) |
| T7.4 | Feed decoded buffer through dithering to EPD | Image renders recognisably on display |

---

## T8 — RTC & Alarm Wakeup

**Goal**: Device sleeps and wakes at the correct time.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T8.1 | Set PCF85063 time via I2C, read back | Read matches written value ±1 s |
| T8.2 | Set 60-second alarm, enter deep sleep | Device wakes within 65 s |
| T8.3 | Confirm wakeup cause = RTC alarm | Log shows `ESP_SLEEP_WAKEUP_EXT0` or equivalent |
| T8.4 | Image index persists across sleep | Index in RTC RAM matches pre-sleep value |
| T8.5 | 10× consecutive wake-sleep cycles | All 10 succeed, index increments correctly |

---

## T9 — Full System Integration

**Goal**: End-to-end: wake → select image → dither → display → sleep.

| Step | Action | Pass Criterion |
|------|--------|----------------|
| T9.1 | Put 5 JPEGs on SD card | Device displays each one in sequence over 5 wake cycles |
| T9.2 | Pull power mid-refresh | Device recovers cleanly on next boot (no display corruption) |
| T9.3 | Empty SD card | Device logs error, displays error frame or last image, sleeps |
| T9.4 | 24-hour soak: device wakes every hour | All 24 cycles succeed, correct images displayed |

---

## T10 — Power Budget

**Goal**: Validate battery life target.

| Step | Measurement | Target |
|------|------------|--------|
| T10.1 | Sleep current (USB power removed, battery only) | < 100 µA |
| T10.2 | Wake + display cycle duration | < 90 s |
| T10.3 | Peak wake current | Document (expect ~200–500 mA during EPD refresh) |
| T10.4 | Daily mAh: sleep_µA × 23.97h + wake_mAh | < 15 mAh/day |
| T10.5 | Projected life on 2000 mAh cell | > 90 days |

**Measurement method**: USB power meter (e.g. Nordic PPK2 or bench supply with current log)
in series with battery connector.
