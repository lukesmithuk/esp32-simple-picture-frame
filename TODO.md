# TODO

Granular task list. Items are grouped by phase and ordered by priority within each group.
Move completed items to PROGRESS.md.

## Phase 1 — Hardware Bring-Up ✓ COMPLETE (all items verified 2026-02-21)

- [x] Scaffold ESP-IDF project structure (CMakeLists.txt, sdkconfig.defaults)
- [x] Set target esp32s3, verify toolchain builds
- [x] Write I2C scan utility (0x01–0x77, log all found devices)
- [x] Flash I2C scan — confirmed 0x34 (TG28), 0x51 (PCF85063), 0x70 (SHTC3)
- [x] Read TG28 chip ID (reg 0x03) — returned 0x4A (not 0x47; register-compatible confirmed)
- [x] Extended register probe (14 regs 0x00–0x41) — all readable, no hangs
- [x] Register write test (IRQ_EN_1 0x40) — write/readback/restore PASS
- [x] Pull schematic from Waveshare wiki — determine:
  - PCF85063 INTB → **GPIO6 directly** (not through TG28)
  - SD card → **4-bit SDIO** (GPIO38/39/40/41/1/2)
- [x] Verify EPD power: enable TG28 ALDO3 via I2C → ALDO3 already live (LDO_EN_2=0x0C), ALDO3_VOLT=3.3V, BUSY=HIGH (EPD idle) ✓
- [x] Mount SD card (4-bit SDIO: GPIO38/39/40/41/1/2), list root directory — 14.9GB SDHC, /sdcard mounted, images/ dir present ✓

## Phase 2 — PMIC Driver (pure C) ✓ COMPLETE (2026-02-22)

- [x] Implement pmic_init(): open I2C device handle, verify chip ID (0x4A)
      Note: AXP2101 also has ID 0x4A — the earlier "AXP2101 = 0x47" was wrong. See ADR-013.
- [x] Implement pmic_epd_power(): enable/disable ALDO3 (EPD_VCC) via LDO_EN_2 bit2
- [x] Implement pmic_sleep(): write 0x00 to LDO_EN_1/2/3; DCDC_EN left untouched (see below)
- [x] Implement pmic_run_tests(): chip ID, register probe, write test, ALDO3 power cycle
- [x] Application skeleton: main.c restructured with boot-cycle model (ADR-012)
- [x] Test mode: CONFIG_TEST_MODE Kconfig option + tests.c framework
- [x] Component structure: components/pmic/ with CMakeLists.txt
- [x] Verify wakeup chain from schematic: PCF85063 INTB → **GPIO6 directly** (confirmed from schematic)
- [x] Verify production boot cycle on hardware — pmic_init + EPD power cycle confirmed working
- [x] Refactor sleep sequence into enter_deep_sleep(pmic, bus) helper in main.c (ADR-014)
- [x] Document flash/boot procedure: cold USB plug required for clean I2C; BOOT+USB replug
      leaves I2C in bad state; esptool hard-reset via USB-JTAG lands in download mode
- [x] flash.py: add --timeout passthrough to monitor.py; flush stdout before os.execv

## Phase 3 — EPD Driver

Init sequence and pixel format are fully known from Waveshare Jan 2026 source (see PROGRESS.md).

- [ ] Implement SPI init: MOSI=11, CLK=10, CS=9, DC=8, RST=12, BUSY=13, 40 MHz, half-duplex
- [ ] Implement EPD reset: RST high 50ms → low 20ms → high 50ms
- [ ] Implement EPD init sequence (exact bytes in PROGRESS.md)
- [ ] Implement BUSY wait: poll GPIO 13 until HIGH (active low, returns when idle)
- [ ] Implement frame send: cmd 0x10 → DMA send 192,000 bytes → display refresh sequence
- [ ] Implement display refresh: 0x04 → wait → 0x06+data → 0x12 00 → wait → 0x02 00 → wait
- [ ] Implement panel sleep command (verify command byte from Waveshare source)
- [ ] Test: all-white frame (index=1 packed: 0x11 repeated)
- [ ] Test: all-black frame (index=0 packed: 0x00 repeated)
- [ ] Test: solid colour for each of the 6 palette colours
- [ ] Measure EPD refresh time with stopwatch (expect ~30s)

## Phase 4 — Image Pipeline

- [ ] Find measured Spectra 6 palette in aitjcize epaper component (search `components/epaper/`)
- [ ] Implement Floyd-Steinberg dithering in C against measured palette
  - Working buffer MUST be `heap_caps_malloc(MALLOC_CAP_SPIRAM)` — 800×480×3 = 1.15 MB
  - Input: RGB888 buffer; output: 4bpp packed EPD buffer
- [ ] Write host-side unit test (Linux): feed synthetic gradient, write PPM for visual check
- [ ] Add `esp_jpeg` component to project (ESP-IDF built-in, no external dep needed)
- [ ] Implement JPEG decode: `esp_jpeg_decode_one_picture()` → RGB888 in PSRAM
- [ ] Implement bilinear scale (fixed-point ×1024, 4-neighbour) to fit 800×480
  - Handle landscape (scale to 800×480) and portrait (scale to 480×800, rotate)
- [ ] Define portrait image policy: letterbox with white bars, or crop-to-fill
- [ ] End-to-end test: JPEG on SD → decode → scale → dither → EPD display

## Phase 5 — Wake / Sleep

PCF85063 time read/write available from aitjcize (port pcf85063.c). Alarm support missing —
must implement from scratch using PCF85063 datasheet registers 0x0B–0x0F.

- [ ] Port aitjcize pcf85063.c (time read/write, OSF check) — pure C, new i2c_master API
- [ ] Implement PCF85063 alarm registers:
  - 0x0B: seconds alarm (AEN bit 7)
  - 0x0C: minutes alarm (AEN bit 7)
  - 0x0D: hours alarm (AEN bit 7)
  - 0x0E: day alarm (AEN bit 7)
  - 0x0F: weekday alarm (AEN bit 7)
  - Clear alarm flag in Control_2 (reg 0x01, AF bit)
- [x] Determine wakeup GPIO from schematic: **GPIO6** (PCF85063 INT direct, confirmed)
- [x] Implement EXT0 wakeup config on GPIO6 (PCF85063 INT, active LOW) — in enter_deep_sleep()
- [ ] Implement deep sleep: remove debug halt in main.c, call enter_deep_sleep() directly
      (blocked on ADR-014: must map LDO_EN_3 DLDO1/DLDO2 rails first — see Phase 7)
- [ ] Implement TG28 sleep sequence in C (adapt aitjcize axp2101_basic_sleep_start logic)
- [ ] Test: set 60s alarm, enter deep sleep, verify wakeup and alarm cause logged
- [ ] Persist image index in RTC fast memory (8KB, survives deep sleep)
- [ ] Handle first-boot: RTC RAM magic word check to detect uninitialised state

## Phase 6 — WiFi (deferred)

- [ ] WiFi STA connect with configurable SSID/password (NVS)
- [ ] HTTP GET raw image URL
- [ ] Fallback to SD on HTTP failure

## Phase 7 — Power / Battery Life

- [ ] Map LDO_EN_3 (reg 0x13) DLDO1/DLDO2 rail assignments from schematic
      Boot-time value: 0x03 (DLDO1 bit0 + DLDO2 bit1). pmic_sleep() zeros this,
      which silences USB-JTAG serial output (ADR-014). Must identify what each rail
      powers before enabling pmic_sleep() in production. Debug build halts before
      pmic_sleep() until resolved.
- [ ] Determine DCDC_EN (reg 0x10) bit→rail mapping on TG28
      Boot-time value: 0x34 (bits 2, 4, 5 set).  DC1 (3.3V system) must be on,
      but bit0=0, so DC1 does not map to bit0.  Mapping unclear — touching this
      register risks killing the system rail.  Once confirmed, add DCDC disable
      to pmic_sleep() to cut DC2–DC5 and reduce sleep current further.
- [ ] Measure sleep current (target: <100 µA)
- [ ] Measure peak wake current and wake duration
- [ ] Calculate estimated daily mAh consumption
- [ ] Project battery life against known cell capacity
- [ ] Tune PMIC output voltages if TG28 allows it (register map now confirmed)

## Backlog / Nice-to-Have

- [ ] OTA firmware update via WiFi
- [ ] Simple web UI for image upload to SD
- [ ] Temperature/humidity display overlay (SHTC3)
- [ ] Low-battery indicator on EPD
