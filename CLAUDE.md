# E-Ink Photo Frame Project

## Overview

Battery-powered e-ink picture frame that wakes once per day, updates the displayed image (from SD card or WiFi), and returns to deep sleep. Target is months of battery life on a single charge.

## Hardware: Waveshare ESP32-S3-PhotoPainter

- **Product page**: https://www.waveshare.com/esp32-s3-photopainter.htm
- **Wiki**: https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter
- **Waveshare repo**: https://github.com/waveshareteam/ESP32-S3-PhotoPainter
- **Schematic**: linked from wiki page

### Key Components

| Component | Part | Notes |
|-----------|------|-------|
| MCU | ESP32-S3-WROOM-1-N16R8 | 240MHz dual-core, 16MB Flash, 8MB PSRAM |
| Display | 7.3" E Ink Spectra 6 (E6) | 800×480, 6 primary colours + dithering, ~30s refresh |
| PMIC | **TG28** (newer revision) | Replaces AXP2101 on older boards. NOT register-compatible confirmed — needs investigation |
| RTC | PCF85063 | I2C, alarm-based wakeup capability |
| Temp/Humidity | SHTC3 | I2C |
| Audio ADC | ES7210 | Dual microphone array (not needed for this project) |
| Audio DAC | ES8311 | Speaker output (not needed for this project) |
| Storage | TF (microSD) card slot | FAT32 format |
| Battery | 3.7V LiPo via MX1.25 2-pin header | Optional, with onboard charge management |
| RTC Backup | MX1.25 2-pin header | Rechargeable RTC battery only |

### GPIO Pin Mapping

| Function | GPIO Pin |
|----------|----------|
| SPI MOSI (EPD) | 11 |
| SPI CLK (EPD) | 10 |
| EPD CS | 9 |
| EPD DC | 8 |
| EPD RST | 12 |
| EPD BUSY | 13 |
| EPD PWR | 6 |
| I2C SDA | 47 |
| I2C SCL | 48 |

### I2C Bus Devices

| Device | Expected Address (7-bit) | Notes |
|--------|--------------------------|-------|
| TG28 PMIC | 0x34 (if AXP2101-compatible) | **Needs verification via I2C scan** |
| PCF85063 RTC | 0x51 | Standard address |
| SHTC3 | 0x70 | Temp/humidity sensor |

### Known Issues

- **TG28 PMIC**: This board has the newer TG28 chip, NOT the AXP2101. All existing open-source firmware (aitjcize/esp32-photoframe, Waveshare demo, XPowersLib) targets the AXP2101. The TG28 may be register-compatible — verify by I2C scan and reading chip ID register 0x03 (AXP2101 returns 0x47). No public TG28 datasheet found yet.
- **The TG28 fixes the AXP2101 dual-power bug**: can safely use USB-C and battery simultaneously.
- **EPD power pin (GPIO 6)**: must be driven high to power the e-paper display. Ensure this is set before SPI comms.
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
# Source environment (add alias to .bashrc: alias get_idf='. $HOME/esp/esp-idf/export.sh')
get_idf

# Set target (once per project)
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyACM0 flash monitor

# Exit monitor: Ctrl+]
```

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
