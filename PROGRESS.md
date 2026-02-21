# Progress Log

Completed work, findings, and session notes. Newest entries at the top.

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
