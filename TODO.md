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

## Phase 2 — PMIC Driver (pure C)

TG28 is AXP2101 register-compatible (confirmed). Plan: port aitjcize axp2101_basic_sleep_start
logic to pure C; patch chip ID check to accept 0x4A in addition to 0x47.

- [ ] Implement pmic_init(): open I2C device handle, verify chip ID (accept 0x4A or 0x47)
- [ ] Implement pmic_sleep(): disable non-essential rails, keep DC1 (3.3V system)
  - Disable: DC2–5, ALDO1–2, BLDO1–2, CPUSLDO, DLDO1–2, ALDO3, ALDO4
  - Keep: DC1 (3.3V) — adapt register values from aitjcize axp2101_basic_sleep_start
- [x] Verify wakeup chain from schematic: PCF85063 INTB → **GPIO6 directly** (confirmed from schematic)

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
- [ ] Implement deep sleep with EXT0/EXT1 wakeup on GPIO6 (PCF85063 INT, active LOW)
- [ ] Implement TG28 sleep sequence in C (adapt aitjcize axp2101_basic_sleep_start logic)
- [ ] Test: set 60s alarm, enter deep sleep, verify wakeup and alarm cause logged
- [ ] Persist image index in RTC fast memory (8KB, survives deep sleep)
- [ ] Handle first-boot: RTC RAM magic word check to detect uninitialised state

## Phase 6 — WiFi (deferred)

- [ ] WiFi STA connect with configurable SSID/password (NVS)
- [ ] HTTP GET raw image URL
- [ ] Fallback to SD on HTTP failure

## Phase 7 — Power / Battery Life

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
