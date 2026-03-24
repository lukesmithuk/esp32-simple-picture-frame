# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 e-paper picture frame firmware for the Waveshare ESP32-S3-PhotoPainter board. The device wakes from deep sleep on an RTC alarm, fetches a JPEG from a WiFi photo server (or SD card fallback), decodes and dithers it to the 6-colour Spectra 6 (E6) e-paper palette, renders it, then sleeps again. ESP-IDF v5.5.3. Primarily C; C++ components are acceptable where useful (e.g. third-party drivers).

All I2C is bit-banged (~100 kHz). The IDF v5.5.3 I2C master driver on ESP32-S3 fires corrupt SCL clear-bus pulses on transaction timeouts, permanently wedging the PMIC after RTS-triggered reset.

## Hardware: Waveshare ESP32-S3-PhotoPainter

- **Product page**: https://www.waveshare.com/esp32-s3-photopainter.htm
- **Wiki**: https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter
- **Waveshare repo**: https://github.com/waveshareteam/ESP32-S3-PhotoPainter
- **Schematic**: `hardware/photopainter-schematic.pdf` (also at https://files.waveshare.com/wiki/ESP32-S3-PhotoPainter/ESP32-S3-PhotoPainter-Schematic.pdf)

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

## Server

Python/FastAPI photo server (in `server/`). See `server/install.sh` for setup.

```bash
cd server && ./install.sh          # create venv + install deps
PHOTOFRAME_API_KEY=yourkey ./run.sh # start for testing
./install-service.sh               # install as systemd service
```

**Install from release** (no git needed):
```bash
curl -L https://github.com/lukesmithuk/esp32-simple-picture-frame/releases/latest/download/photoframe-server.tar.gz | tar xz
cd photoframe-server && ./setup.sh
```

**Uninstall:** `./uninstall.sh` (removes systemd service, optionally deletes data)

**Run tests:**
```bash
cd server && source venv/bin/activate && python -m pytest tests/ -v
```

## Component Map

| Component | Purpose | Key dependencies |
|-----------|---------|-----------------|
| `board` | I2C bus, AXP2101 PMIC, PCF85063 RTC | `esp_hw_support`, `esp_driver_i2c` |
| `epd` | SPI EPD driver (800x480, 6-colour Spectra 6, 4bpp) | `esp_driver_spi` |
| `sdcard` | 4-bit SDIO mount/unmount at `/sdcard` | `fatfs`, `sdmmc`, `esp_driver_sdmmc` |
| `image_picker` | Directory scan + shuffle without repeat | `esp_hw_support` |
| `image_loader` | File → PSRAM buffer (4MB max) | `esp_hw_support` |
| `image_decode` | JPEG decode → scale → CDR → dither → 4bpp frame buffer | `esp_hw_support`, `espressif/esp_jpeg` |
| `epd_text` | 8x8 bitmap font renderer for 4bpp frame buffer | `epd` (constants only) |
| `applog` | ESP_LOG tee to SD card file + explicit log writes | (none) |
| `config` | Key=value config file reader from SD card | (none) |
| `wifi_fetch` | WiFi connect, NTP sync, HTTP image/status/log client | `esp_wifi`, `esp_http_client`, `board` |

## IDF v5 Component Names

Use these in `CMakeLists.txt` `REQUIRES`:
- `driver` (covers I2C, SPI, GPIO, etc.)
- `esp_driver_gpio`, `esp_driver_i2c`, `esp_driver_spi` (explicit sub-drivers)
- `esp_hw_support` (contains `esp_sleep.h`, `esp_heap_caps.h`)
- `fatfs`, `sdmmc`, `esp_driver_sdmmc` (SD card)

Do NOT add `esp_log` or `freertos` to `REQUIRES` — they are auto-linked in IDF v5.5.

## EPD Panel — Spectra 6 (E6)

6-colour display: Black, White, Yellow, Red, Blue, Green. **No Orange.**

Panel colour indices (from aitjcize `epaper.h`):
- 0=Black, 1=White, 2=Yellow, 3=Red, 4=Clean (reserved), 5=Blue, 6=Green
- `epd_color_t` enum has a gap at index 4

Panel scans bottom-right origin — `epd_display()` rotates the frame buffer 180° before SPI transfer.

## Image Decode Pipeline

- JPEG decode via `espressif/esp_jpeg` managed component (TJpgDec, ROM-based on ESP32-S3)
- **Baseline JPEG only** — progressive JPEG not supported (TJpgDec limitation)
- Cover-mode nearest-neighbor scale to 800×480 (fill display, centre-crop excess)
- Floyd-Steinberg dither with **measured** palette values (not theoretical sRGB)
- Measured palette: Black(2,2,2) White(190,200,200) Yellow(205,202,0) Red(135,19,0) Blue(5,64,158) Green(39,102,60)

## Config File (`/sdcard/config.txt`)

Key=value format, `#` comments. All keys optional.

| Key | Default | Purpose |
|-----|---------|---------|
| `wake_interval_hours` | 1 | Hours between updates |
| `wake_interval_minutes` | 0 | Minutes between updates |
| `wake_interval_seconds` | 0 | Seconds between updates |
| `wifi_ssid` | *(none)* | WiFi network (absent = WiFi skipped) |
| `wifi_password` | `""` | WiFi password |
| `server_url` | `""` | Photo server URL |
| `server_api_key` | `""` | API key for server |
| `log_max_size_kb` | 256 | Log file rotation threshold |

## Gotchas

- **sdkconfig.defaults changes** require `rm -rf build sdkconfig && idf.py build` to take effect
- **16MB flash**: Board has 16MB flash — set in sdkconfig.defaults, uses `single_app_large` partition
- **Sensitive config keys**: `password` and `key` values are masked in log output
- **Deep sleep debug**: Enable `CONFIG_DISABLE_DEEP_SLEEP` in menuconfig to prevent sleep during development

## Reference Implementations

- `aitjcize/esp32-photoframe` — component structure, PCF85063 driver, AXP2101 driver, EPD init/refresh sequence, measured palette values, image processing pipeline
- `waveshareteam/ESP32-S3-PhotoPainter` — authoritative EPD init bytes, JPEG/PNG/BMP pipeline
