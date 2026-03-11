# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 e-paper picture frame firmware for the Waveshare ESP32-S3-PhotoPainter board. The device wakes from deep sleep on an RTC alarm, loads an image from SD card, dithers it to the 7-colour e-paper palette, renders it, then sleeps again. ESP-IDF v5.5.3. Primarily C; C++ components are acceptable where useful (e.g. third-party drivers).

All I2C is bit-banged (~100 kHz). The IDF v5.5.3 I2C master driver on ESP32-S3 fires corrupt SCL clear-bus pulses on transaction timeouts, permanently wedging the PMIC after RTS-triggered reset.

## Hardware: Waveshare ESP32-S3-PhotoPainter

- **Product page**: https://www.waveshare.com/esp32-s3-photopainter.htm
- **Wiki**: https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter
- **Waveshare repo**: https://github.com/waveshareteam/ESP32-S3-PhotoPainter
- **Schematic**: `hardware/ESP32-S3-PhotoPainter-Schematic.pdf` (also at https://files.waveshare.com/wiki/ESP32-S3-PhotoPainter/ESP32-S3-PhotoPainter-Schematic.pdf)

## Build System

**Environment (must source before any idf.py command):**
```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
```

**Build:**
```bash
idf.py build
```

**Flash + monitor (normal workflow):**
```bash
python3 flash.py --timeout 30
```
`flash.py` uses `--before default-reset` so esptool handles download mode entry
automatically — no BOOT button sequence required.

**Flash only (no monitor):**
```bash
python3 flash.py --no-monitor
```

**Monitor only (attach without reset):**
```bash
python3 monitor.py --timeout 30
```

**menuconfig:**
```bash
idf.py menuconfig
```

## IDF v5 Component Names

Use these in `CMakeLists.txt` `REQUIRES`:
- `driver` (covers I2C, SPI, GPIO, etc.)
- `esp_driver_gpio`, `esp_driver_i2c`, `esp_driver_spi` (explicit sub-drivers)
- `esp_hw_support` (contains `esp_sleep.h`)

Do NOT add `esp_log` or `freertos` to `REQUIRES` — they are auto-linked in IDF v5.5.

## Reference Implementations

- `aitjcize/esp32-photoframe` — component structure, PCF85063 driver, AXP2101 driver, EPD init/refresh sequence (`driver_ed2208_gca.c`)
- `waveshareteam/ESP32-S3-PhotoPainter` — authoritative EPD init bytes, JPEG/PNG/BMP pipeline
