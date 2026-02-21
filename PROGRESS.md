# Progress Log

Completed work, findings, and session notes. Newest entries at the top.

---

## 2026-02-21 — Phase 1 bring-up: extended register probe + write test

### I2C hang root cause identified and fixed

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

### TG28 chip ID: **0x4A** (NOT 0x47)

Register 0x03 returned `0x4A`. AXP2101 returns `0x47`. The TG28 is **not** AXP2101-compatible.

This means:
- XPowersLib cannot be used, even as a reference implementation
- All PMIC register writes must be skipped unless/until the TG28 register map is found
- Minimal fallback: skip PMIC entirely; EPD power is on GPIO 6 (independent of PMIC)
- Battery status, charging control, and regulated rail control are unavailable for now

Next steps for PMIC: search for TG28 datasheet or any community reverse-engineering. The chip ID `0x4A` is not found in any known open-source PMIC library.

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
