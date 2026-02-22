# E-Ink Photo Frame Project

## Overview

Battery-powered e-ink picture frame that wakes once per day, updates the displayed image (from SD card or WiFi), and returns to deep sleep. Target is months of battery life on a single charge.

## Hardware: Waveshare ESP32-S3-PhotoPainter

- **Product page**: https://www.waveshare.com/esp32-s3-photopainter.htm
- **Wiki**: https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter
- **Waveshare repo**: https://github.com/waveshareteam/ESP32-S3-PhotoPainter
- **Schematic**: `hardware/ESP32-S3-PhotoPainter-Schematic.pdf` (also at https://files.waveshare.com/wiki/ESP32-S3-PhotoPainter/ESP32-S3-PhotoPainter-Schematic.pdf)

### Key Components

| Component | Part | Notes |
|-----------|------|-------|
| MCU | ESP32-S3-WROOM-1-N16R8 | 240MHz dual-core, 16MB Flash, 8MB PSRAM |
| Display | 7.3" E Ink Spectra 6 (E6) | 800×480, 6 primary colours + dithering, ~30s refresh |
| PMIC | **TG28** (newer revision) | Replaces AXP2101 on older boards. **Register-compatible confirmed** (2026-02-21): chip ID 0x4A, write test passed. Port aitjcize AXP2101 driver; patch chip ID check to accept 0x4A. |
| RTC | PCF85063 | I2C, alarm-based wakeup capability |
| Temp/Humidity | SHTC3 | I2C |
| Audio ADC | ES7210 | Dual microphone array (not needed for this project) |
| Audio DAC | ES8311 | Speaker output (not needed for this project) |
| Storage | TF (microSD) card slot | FAT32 format |
| Battery | 3.7V LiPo via MX1.25 2-pin header | Optional, with onboard charge management |
| RTC Backup | MX1.25 2-pin header | Rechargeable RTC battery only |

### GPIO Pin Mapping

| Function | GPIO Pin | Notes |
|----------|----------|-------|
| SPI MOSI (EPD) | 11 | |
| SPI CLK (EPD) | 10 | |
| EPD CS | 9 | |
| EPD DC | 8 | |
| EPD RST | 12 | |
| EPD BUSY | 13 | Active LOW — poll until HIGH |
| RTC_INT (PCF85063) | 6 | Direct GPIO, NOT through TG28. Use as EXT0/EXT1 deep-sleep wakeup source |
| I2C SDA | 47 | Shared by TG28, PCF85063, SHTC3, ES8311, ES7210 |
| I2C SCL | 48 | |
| AXP_IRQ (TG28) | 21 | TG28 interrupt output |
| SYS_OUT (TG28) | 5 | AXP2101 system power indicator |
| CHGLED | 3 | Charging LED indicator |
| SD_CS / D3 | 38 | 4-bit SDIO |
| SD_CLK | 39 | 4-bit SDIO |
| SD_D0 / MISO | 40 | 4-bit SDIO |
| SD_CMD / MOSI | 41 | 4-bit SDIO |
| SD_D1 | 1 | 4-bit SDIO (via 0Ω R60) |
| SD_D2 | 2 | 4-bit SDIO (via 0Ω R59) |

**EPD power**: Supplied by TG28 ALDO3 rail (enable/disable via I2C). No dedicated EPD_PWR GPIO.

### I2C Bus Devices

| Device | Expected Address (7-bit) | Notes |
|--------|--------------------------|-------|
| TG28 PMIC | 0x34 | **Register-compatible with AXP2101** (chip ID 0x4A, write test passed 2026-02-21) |
| PCF85063 RTC | 0x51 | Standard address |
| SHTC3 | 0x70 | Temp/humidity sensor |

### Known Issues

- **TG28 PMIC**: Register-compatible with AXP2101 (confirmed 2026-02-21). Chip ID reg 0x03 = 0x4A (AXP2101 = 0x47). Register read/write tests passed across 0x00–0x41. Plan: port aitjcize AXP2101 C++ driver to pure C, patch chip ID check to also accept 0x4A. XPowersLib `begin()` will reject 0x4A — avoid using it directly.
- **The TG28 fixes the AXP2101 dual-power bug**: can safely use USB-C and battery simultaneously.
- **EPD power**: Controlled by TG28 ALDO3 rail via I2C — enable ALDO3 before SPI comms, disable after display sleeps. There is NO dedicated EPD_PWR GPIO (earlier notes saying GPIO6=EPD_PWR were incorrect).
- **Wakeup path**: PCF85063 INT → **GPIO6 directly** (not through TG28). Deep sleep wakeup source = EXT0/EXT1 on GPIO6. The aitjcize firmware assumed wakeup via AXP2101 IRQ — schematic confirms this is wrong for our board.
- **SD card interface**: **4-bit SDIO** (not SPI). Use ESP-IDF SDMMC driver with GPIO38/39/40/41/1/2.
- **Download mode**: hold BOOT button + press PWR to enter download mode if device not detected on USB.

## Development Environment

### Toolchain: ESP-IDF v6.0.0 on Ubuntu 25.10

```bash
# Prerequisites
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
    libdbus-1-dev libdbus-glib-1-dev pkg-config libglib2.0-dev python3-dev

# Serial port access
sudo usermod -aG dialout $USER
# (logout/login required)
```

ESP-IDF installed via EIM (ESP-IDF Installation Manager). Target version: v6.0.0.

### Build Commands

```bash
# Source environment
source /home/pls/espressif/release-v6.0/esp-idf/export.sh
# (or add alias to .bashrc: alias get_idf='source ~/espressif/release-v6.0/esp-idf/export.sh')

# Set target (once per project)
idf.py set-target esp32s3

# Build
idf.py build

# Flash + start serial monitor (preferred — see Helper Scripts below)
python3 flash.py

# Flash only, no monitor
python3 flash.py --no-monitor

# Serial monitor only (after manual reset or separate flash step)
python3 monitor.py --timeout 30   # capture 30 s then exit
python3 monitor.py                # run until Ctrl-C
```

**Why not `idf.py flash monitor`?**  `idf.py monitor` requires an interactive TTY and
exits immediately when run non-interactively (e.g. from a script or Claude Code).
Use `flash.py` + `monitor.py` instead.

**IDF v6 component names** (differ from IDF v5 — use in `REQUIRES` lists): `esp_driver_i2c` (not `driver`), `esp_driver_spi`, `esp_driver_gpio`, `log` (not `esp_log`), `esp_hw_support` (has `esp_sleep.h`), `heap` (for `heap_caps_malloc`). Header `#include` paths are unchanged.

### Helper Scripts

Two small Python scripts wrap esptool and serial I/O for this project.

#### `flash.py`

Reads `build/flash_args` (generated by `idf.py build`), invokes esptool from the
IDF Python environment, then optionally starts `monitor.py` via `os.execv`.

```
python3 flash.py [--port /dev/ttyACM0] [--baud 460800] [--no-monitor]
```

- `--no-monitor` — flash only, do not open serial monitor afterwards.
- Uses the IDF-managed Python at `~/.espressif/python_env/idf6.0_py3.13_env/bin/python`
  so no separate `pip install esptool` is needed.

#### `monitor.py`

Minimal serial monitor using PySerial.  Attaches to the running chip without
resetting it (default), then streams output to stdout until Ctrl-C or a timeout.

```
python3 monitor.py [--port /dev/ttyACM0] [--baud 115200] [--timeout N] [--reset]
```

- `--timeout N` — stop after N seconds (0 = run until Ctrl-C, the default).
- `--reset` — toggle RTS to reset the chip before monitoring.  **WARNING**: the
  bare RTS toggle can leave the I2C bus in a bad state on this board.  Prefer
  `python3 flash.py` (esptool reset + immediate attach) when you need a clean
  boot capture.
- Raw bytes are written to stdout, so IDF ANSI log colours are preserved.
- Pipe to `cat` or redirect to a file to capture output non-interactively.

## Reference Firmware / Code

### aitjcize/esp32-photoframe (recommended reference)
- **Repo**: https://github.com/aitjcize/esp32-photoframe
- Feature-rich replacement firmware for PhotoPainter
- RESTful API, web interface, SD card rotation, URL-based image fetching
- Floyd-Steinberg dithering with **measured** colour palette (not theoretical RGB)
- Deep sleep support
- Companion image server with Google Photos / Synology DS Photos support
- **Caveat**: AXP2101 driver only — needs TG28 adaptation

### Waveshare Official Demo
- **Repo**: https://github.com/waveshareteam/ESP32-S3-PhotoPainter
- Basic ESP-IDF demo code, useful for hardware bring-up
- Check latest commits (Jan 2026) for possible TG28 support

### Community Project (weather dashboard)
- **Repo**: https://github.com/multiverse2011/esp32-s3-photopainter
- ESP-IDF project with component structure: wifi_manager, epd_driver, gfx_library, axpPower, etc.
- Good reference for component architecture

### Waveshare ESP32 Components
- **Repo**: https://github.com/waveshareteam/Waveshare-ESP32-components
- Reusable ESP component library for Waveshare boards (drivers, BSP)

## Architecture Plan

1. **Boot** — init peripherals, read RTC for current time
2. **Decide** — check if update is due (or first boot / manual trigger)
3. **Source** — read next image from SD card rotation, or bring up WiFi and pull from network endpoint
4. **Dither** — Floyd-Steinberg against Spectra 6 palette (6 colours → lookup table, 800×480 framebuffer ~192KB at 4bpp, fits in 8MB PSRAM)
5. **Display** — push framebuffer to EPD via SPI, wait for refresh complete (~30 seconds)
6. **Sleep** — set RTC alarm, power down peripherals, enter deep sleep

## Display: Spectra 6 Colour Palette

The E Ink Spectra 6 display uses 6 primary pigment colours:
- Black
- White
- Green
- Blue
- Red
- Yellow

Images must be dithered to this palette. The aitjcize firmware's **measured palette** approach accounts for how the e-ink display actually renders colours (darker/more muted than pure RGB), producing significantly better results than naive theoretical palette matching.

Waveshare also provides:
- Photoshop-based conversion workflow (using .act palette files)
- "Six-color dithering image conversion tool" (linked from wiki)

## Developer Notes

- **Language**: C (ESP-IDF native). Developer has 20+ years C experience, expert level.
- **No Arduino abstractions** — use ESP-IDF directly for full control over power management and sleep modes.
- **Display refresh**: Colour e-ink refresh takes ~30 seconds. Plan for this in the wake cycle timing.
- **E-paper care**: Set display to sleep mode or power off after refresh. Leaving in high-voltage state can damage the panel.
- **SD card format**: FAT32 only. Use `esp_vfs_fat` ESP-IDF component.
- **WiFi**: Standard ESP-IDF WiFi STA mode for network image fetching.
- **SPI single-byte commands**: use `SPI_TRANS_USE_TXDATA` flag with `t.tx_data = {byte}` rather than `t.tx_buffer = &local_var` — stores the byte in the transaction descriptor, bypasses DMA for ≤4-byte transfers, avoids a bounce-buffer alloc on every command.
- **PSRAM + SPI DMA**: `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` is sufficient for SPI framebuffer transfers on ESP32-S3; no need for `MALLOC_CAP_DMA`. ESP32-S3 EDMA makes PSRAM DMA-accessible; IDF driver handles the descriptor chain automatically.
