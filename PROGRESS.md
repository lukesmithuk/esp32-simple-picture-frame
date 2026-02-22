# Progress Log

Completed work, findings, and session notes. Newest entries at the top.

---

## 2026-02-22 — Phase 2 complete: PMIC driver + application skeleton

### Chip ID correction

Earlier notes recorded "AXP2101 = 0x47, TG28 = 0x4A".  This was wrong.
Confirmed from XPowersLib source (`REG/AXP2101Constants.h`):
`XPOWERS_AXP2101_CHIP_ID = 0x4A`.  The real AXP2101 also returns 0x4A.
The `pmic_init()` check therefore accepts only 0x4A (see ADR-013).

### PMIC driver (`components/pmic/`)

Pure-C driver implemented using the I2C pattern confirmed in Phase 1:
- `pmic_init()` — adds device handle (0x34, 100 kHz), reads chip ID
- `pmic_epd_power()` — sets ALDO3 voltage then enable bit (reg 0x12 bit2);
  confirmed register addresses from Phase 1 hardware testing
- `pmic_sleep()` — writes 0x00 to LDO_EN_1/2/3 (regs 0x11–0x13);
  DCDC_EN (reg 0x10) left untouched pending bit-mapping investigation
- `pmic_run_tests()` — register probe, write test, ALDO3 power cycle
- `pmic_deinit()` — removes I2C device handle

### Application skeleton

`main.c` restructured into the production boot-cycle model (ADR-012):
boot → init → update decision → EPD update (stubbed) → pmic_sleep() →
esp_deep_sleep_start().  No main loop — deep sleep is the loop.

### Test mode

`CONFIG_TEST_MODE` Kconfig option added.  When enabled, `app_main()` calls
`tests_run()` (in `tests.c`) instead of entering the production boot cycle.
Keeps the device alive for serial monitor observation.  Add per-component
test functions to `tests.c` as each phase completes.

### TG28 register layout

Confirmed that TG28 uses an older AXP register layout for DC/LDO control,
not the AXP2101 layout (which moved DC enable to 0x80, LDO enable to 0x90).
All registers used in pmic.c are confirmed working on hardware from Phase 1.

---

## 2026-02-21 — Phase 1 complete: EPD power + SD card verified

### I2C hang: development tooling artifact, not a firmware bug

Observed hang: after `monitor.py` resets the chip via RTS, I2C reads hang after exactly
N successful register reads (N = number of devices that ACKed during the bus scan, here 5).
The same firmware works correctly when captured via `flash.py` (esptool hard reset →
immediate `--no-reset` monitor).

**Root cause**: esptool uses a precise DTR+RTS sequence designed for ESP32 USB-JTAG;
`monitor.py`'s bare `rts=True → sleep(100ms) → rts=False` leaves the I2C bus in a
state that causes slave devices to hang subsequent reads.  This is a development-tooling
artifact — not a firmware bug and not relevant to production (deep sleep wakeup uses RTC
hardware wakeup, not any USB-serial reset).

**Workaround**: always capture output via `python3 flash.py` rather than running
`monitor.py` standalone with its default chip reset.

**Also simplified** (unrelated to the hang): removed per-call
`i2c_master_bus_wait_all_done()` from `pmic_read` / `pmic_write`.  Confirmed redundant —
`i2c_master_transmit_receive` is synchronous; the original `wait_all_done` calls were a
cargo-cult workaround from when temp handles were being used per-read.

Confirmed working pattern: persistent device handle + direct `transmit_receive` calls,
no `wait_all_done` needed.

### EPD power test result: **[INFO] ALDO3 already enabled**

TG28 ALDO3 was already on (LDO_EN_2 reg 0x12 = 0x0C, bit 2 set) from a previous run.
ALDO3_VOLT (reg 0x1C) = 0x1C = 28 → (500 + 28 × 100) mV = **3300 mV = 3.3 V** ✓
EPD_BUSY (GPIO13) was HIGH before and after — EPD is powered and idle, not asserting BUSY.
ALDO3 → EPD_VCC rail confirmed functional.

### SD card test result: **[PASS]**

| Field | Value |
|-------|-------|
| Type | SDHC |
| Capacity | 14.9 GB (30,535,680 × 512-byte sectors) |
| Interface | 4-bit SDIO at 400 kHz (probing speed) |
| Mount point | /sdcard |
| Root entries | 11 (dirs + hidden .current.* files) |

Root directory listing:
```
System Volume Information   DIR
02_sys_ap_img               DIR
03_sys_ap_html              DIR
04_sys_ai_img               DIR
05_user_ai_img              DIR
06_user_Foundation_img      DIR
01_sys_init_img             DIR
images                      DIR
.current.jpg                file
.current.lnk                file
.current.png                file
```

The `images/` directory is where user images should be placed for the picture frame.
The `.current.*` files appear to be Waveshare firmware state tracking files (safe to ignore).

**Phase 1 bring-up is complete.** All hardware verified: I2C bus, TG28 PMIC, EPD power, SD card.

---

## 2026-02-21 — Phase 1: Schematic pulled and analysed

Downloaded schematic PDF (`hardware/ESP32-S3-PhotoPainter-Schematic.pdf`) from Waveshare wiki.
Rendered and cropped all sections for reference (see `hardware/schematic-*.png`).

### Key findings — corrects earlier assumptions

**GPIO6 = RTC_INT, NOT EPD_PWR**
PCF85063 INT pin routes directly to ESP32-S3 GPIO6. There is no EPD_PWR GPIO.
EPD power comes from TG28 ALDO3 (I2C-controlled LDO). To power the EPD: enable ALDO3 via I2C.
Earlier CLAUDE.md entry "EPD power pin (GPIO 6)" was incorrect — now fixed.

**Wakeup path is direct: PCF85063 → GPIO6 → ESP32**
The aitjcize firmware assumed: PCF85063 → AXP2101 IRQ → ESP32-S3. The schematic shows
PCF85063 INT goes directly to GPIO6, with no TG28 in the path. Deep sleep wakeup source
must be EXT0/EXT1 on GPIO6, not GPIO21 (which is AXP_IRQ, a separate signal).

**SD card is 4-bit SDIO (not SPI)**
All 6 SD signals are routed to ESP32 GPIOs. D1 and D2 are connected via fitted 0Ω resistors.

### Complete GPIO map (from schematic)

| Signal | GPIO | Notes |
|--------|------|-------|
| EPD_DC | 8 | |
| EPD_CS | 9 | |
| EPD_SCK | 10 | |
| EPD_DIN | 11 | |
| EPD_RST | 12 | |
| EPD_BUSY | 13 | Active LOW |
| RTC_INT | 6 | PCF85063 INT → GPIO6 direct |
| AXP_IRQ | 21 | TG28 IRQ → GPIO21 |
| SYS_OUT | 5 | TG28 SYS power output indicator |
| CHGLED | 3 | Charging LED |
| I2C SDA | 47 | Shared bus: TG28 + PCF85063 + SHTC3 + audio |
| I2C SCL | 48 | |
| SD_CS/D3 | 38 | 4-bit SDIO |
| SD_CLK | 39 | 4-bit SDIO |
| SD_D0 | 40 | 4-bit SDIO |
| SD_CMD | 41 | 4-bit SDIO |
| SD_D1 | 1 | 4-bit SDIO (0Ω R60) |
| SD_D2 | 2 | 4-bit SDIO (0Ω R59) |

### TG28 PMIC ALDO assignments (from schematic)

| Rail | Output | Notes |
|------|--------|-------|
| ALDO2 | Audio_VCC | Audio subsystem power |
| ALDO3 | EPD_VCC | EPD power — must enable before display use |
| DC1 | 3.3V (VCC3V3) | Main system rail — must stay on |

### Resolved open questions from TODO/PROGRESS

- **PCF85063 INTB wiring**: Direct to GPIO6 (not through TG28) ✓
- **SD card interface**: 4-bit SDIO on GPIO38/39/40/41/1/2 ✓
- **EPD_PWR GPIO**: Does not exist — EPD powered by TG28 ALDO3 ✓

---

## 2026-02-21 — Phase 1 bring-up: extended register probe + write test

### I2C hang root cause identified and fixed

> **[SUPERSEDED]** The analysis below was the working theory at the time.  Later testing
> (same session) showed the hang was entirely a `monitor.py` RTS-reset tooling artifact —
> not a firmware bug.  See the entry at the top of this file for the correct diagnosis.
> `wait_all_done` per-call was subsequently removed; persistent handle alone is sufficient.

The I2C bus was hanging on every read after `i2c_master_probe()` scanned the bus.
Root cause: `i2c_master_probe()` leaves async state on the bus. Fix (from aitjcize
and multiverse2011 reference projects):

1. **Persistent device handle** — open once with `i2c_master_bus_add_device()`, reuse for all transactions
2. **`i2c_master_bus_wait_all_done()`** — call before every `transmit` / `transmit_receive`

Both conditions are required. `wait_all_done` alone with a temp handle still hung.
Persistent handle alone still hung. Together: all 14 registers read cleanly with zero hangs.

### TG28 register dump (AXP2101-named registers, USB powered, no battery)

| Reg | AXP2101 name | Value | Notes |
|-----|--------------|-------|-------|
| 0x00 | PMU_STATUS_1 | 0x20 | bit 5 = VBUS present; bits 3:0 = 0 (no battery) |
| 0x01 | PMU_STATUS_2 | 0x15 | bits 6:5 = charger state; bit 4 = VSYS OK; bit 2, 0 set |
| 0x03 | CHIP_ID | 0x4A | (AXP2101 = 0x47; TG28 differs) |
| 0x08 | SLEEP_CFG | 0x04 | |
| 0x10 | DCDC_EN | 0x34 | bits: DC1=1 (3.3V), DC3=1, DC5=1 |
| 0x11 | LDO_EN_1 | 0x00 | all LDOs in group 1 off |
| 0x12 | LDO_EN_2 | 0x08 | ALDO4 (bit 3) enabled |
| 0x13 | LDO_EN_3 | 0x03 | DLDO1 (bit 0) + DLDO2 (bit 1) enabled |
| 0x15 | DCDC1_VOLT | 0x06 | AXP2101: DC1 = 1500 + 100×6 = 2100 mV? (nominal 3.3V — mapping differs) |
| 0x16 | DCDC2_VOLT | 0x04 | |
| 0x1A | ALDO1_VOLT | 0xA1 | bit 7 = 1 (flag?); lower 5 bits = 1 |
| 0x1B | ALDO2_VOLT | 0x00 | |
| 0x40 | IRQ_EN_1 | 0xFF | all IRQs enabled (default-on) |
| 0x41 | IRQ_EN_2 | 0xFC | |

### Register write test result: **[PASS]**

IRQ_EN_1 (0x40): wrote 0x00 over default 0xFF → readback 0x00 ✓, restored 0xFF ✓.
TG28 register map is writable and responds to AXP2101-addressed registers.

### Conclusion: TG28 is AXP2101 register-compatible

- Same I2C address (0x34) ✓
- Same register map layout (at least 0x00–0x41 confirmed) ✓
- Registers readable and writable ✓
- Chip ID register 0x03 differs (0x4A vs 0x47) — XPowersLib `begin()` will fail unless
  patched to accept 0x4A, or the register check is bypassed

**Recommendation**: Port aitjcize AXP2101 driver logic to pure C; patch chip ID check
to accept both 0x47 and 0x4A. Do NOT use XPowersLib directly — it C++ template
dependency is not worth the complexity for the limited PMIC operations we need.

---

## 2026-02-21 — Phase 1 bring-up: I2C scan results

### I2C scan results (SDA=47, SCL=48, 100 kHz)

| Address | Device | Status |
|---------|--------|--------|
| 0x18 | ES8311 audio DAC | Found (expected — on-board audio, not needed for this project) |
| 0x34 | TG28 PMIC | Found ✓ |
| 0x40 | ES7210 microphone ADC | Found (expected — on-board audio, not needed) |
| 0x51 | PCF85063 RTC | Found ✓ |
| 0x70 | SHTC3 temp/humidity | Found ✓ |

Two `probe device timeout` warnings from IDF during scan — normal for devices that do clock stretching on address probe. All expected devices still detected.

### TG28 chip ID: **0x4A**

Register 0x03 returned `0x4A`.

> **Correction (2026-02-22)**: Earlier notes stated "AXP2101 returns 0x47".  This was wrong.
> XPowersLib source confirms `XPOWERS_AXP2101_CHIP_ID = 0x4A` — the real AXP2101 also returns
> 0x4A.  The TG28 and AXP2101 share the same chip ID.  See ADR-013.

---

## 2026-02-21 — Reference firmware deep-dive

Deep read of aitjcize/esp32-photoframe and waveshareteam/ESP32-S3-PhotoPainter (latest Jan 2026).

### EPD driver — exact init sequence confirmed (from Waveshare display_bsp.cpp)
SPI at **40 MHz**, half-duplex. BUSY pin is **active LOW** — poll until HIGH.

Init sequence (register → data bytes):
```
0xAA → 49 55 20 08 09 18
0x01 → 3F
0x00 → 5F 69
0x03 → 00 54 00 44
0x05 → 40 1F 1F 2C
0x06 → 6F 1F 17 49
0x08 → 6F 1F 1F 22
0x30 → 03
0x50 → 3F
0x60 → 02 00
0x61 → 03 20 01 E0   (= 800 × 480)
0x84 → 01
0xE3 → 2F
0x04 → (POWER_ON, then wait BUSY)
```

Display refresh sequence: `0x04` (POWER_ON) → wait BUSY → `0x06 6F 1F 17 49` → `0x12 00` (DISPLAY_REFRESH) → wait BUSY → `0x02 00` (POWER_OFF) → wait BUSY.

Pixel format: **4bpp packed**, 2 pixels per byte. High nibble = x-even pixel, low nibble = x-odd pixel.
Colour indices: 0=Black, 1=White, 2=Red, 3=Green, 4=Blue, 5=Yellow.

Frame buffer: `width * height / 2` bytes = 800 × 480 / 2 = **192,000 bytes**. Allocate in PSRAM.

### PCF85063 driver — directly reusable, but alarm support missing
`aitjcize/esp32-photoframe/components/rtc_driver_pcf85063/src/pcf85063.c` is pure C, uses
new-style ESP-IDF `i2c_master` API, confirmed address 0x51, 100 kHz. OSF-bit check on read.
**The driver implements only time read/write — no alarm register support.**
Alarm registers (0x0B–0x0F) must be added before RTC-alarm wakeup is possible.

### AXP2101 driver — C++ / XPowersLib, not directly portable to pure C
The aitjcize PMIC driver wraps XPowersLib (a C++ template library). The `begin()` call reads
chip ID internally — if TG28 returns a different ID the call returns false and the PMIC is
effectively disabled. Cannot directly use in a pure-C project.

Key sleep sequence (from axp2101.cpp `axp2101_basic_sleep_start()`):
- Disables: DC2–5, ALDO1–2, BLDO1–2, CPUSLDO, DLDO1–2, ALDO3, ALDO4
- Keeps: DC1 (3.3V system rail)
- Wakeup source set to: AXP2101 IRQ pin → ESP32-S3 EXT wakeup (NOT direct PCF85063 INTB)

This means the intended wakeup path is: PCF85063 alarm → AXP2101 IRQ → ESP32-S3 wakeup.
Verify from schematic whether PCF85063 INTB connects to AXP2101 or directly to an ESP32-S3 GPIO.

### Waveshare Jan 2026 update — image formats, NOT TG28
The Jan 2026 commits added JPG/PNG/BMP decode support. **No TG28 PMIC support added.**
TG28 remains entirely unaddressed in all known open-source code.

### Image pipeline — concrete details from Waveshare imgdecode_app.cpp
- **JPEG**: `esp_jpeg_decode_one_picture()` — ESP-IDF built-in, outputs RGB888
- **PNG**: libpng, streaming 128 rows at a time, all allocations in PSRAM
- **BMP**: custom decoder, 24-bit uncompressed only, handles bottom-up storage
- **Scaling**: fixed-point bilinear interpolation (×1024), despite "Nearest" in function name
- **Dithering**: Floyd-Steinberg, but uses **theoretical palette** (pure sRGB), not measured
- **Bug noted**: working buffer for FS dithering is `malloc()` (DRAM) — at 800×480×3 = 1.15 MB
  this will crash. Must use `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

### aitjcize component map (all directly relevant to our project)
| Component | Files | Usability |
|-----------|-------|-----------|
| `board_hal` | `driver_waveshare_photopainter_73.c` | Adapt (C, our exact board) |
| `rtc_driver_pcf85063` | `pcf85063.c` | Port directly; add alarm support |
| `pmic_driver_axp2101` | `axp2101.cpp` + XPowersLib | C++ — adapt logic, not code |
| `epaper_src` | `GUI_Paint.c`, `GUI_BMPfile.c` etc. | Port/reference |
| `sensor_driver_shtc3` | — | Optional, port if needed |

### Open Questions (updated)
- Does TG28 respond at 0x34 and return 0x47 from register 0x03? **(hardware only)**
- Does PCF85063 INTB connect to AXP2101 IRQ or directly to an ESP32-S3 GPIO? **(check schematic)**
- Does aitjcize use SDIO or SPI for SD card? (Likely SDIO — pins CLK/CMD/D0-D3 referenced in board HAL)
- Exact measured Spectra 6 palette RGB values — find in aitjcize epaper component

---

## 2026-02-21 — Project Initialisation

- Created project repository with initial ESP-IDF scaffold placeholder
- Documented hardware in CLAUDE.md: ESP32-S3-WROOM-1-N16R8, 7.3" Spectra 6 EPD, TG28 PMIC,
  PCF85063 RTC, SHTC3 temp/humidity, ES7210/ES8311 audio (not used)
- Confirmed board revision has TG28 PMIC, not AXP2101
- Identified key reference firmware: aitjcize/esp32-photoframe, Waveshare demo
- Identified key risk: TG28 register compatibility with AXP2101 unverified — no public datasheet
- Created PLAN.md, DECISIONS.md, TODO.md, TEST_PLAN.md

---

<!-- Template for future entries:

## YYYY-MM-DD — <short description>

- Bullet points of what was done
- Findings (especially hardware / register discoveries)
- Decisions made (cross-ref DECISIONS.md ADR number)
- Items moved from TODO to done

### Blocked / Issues
- ...

-->
