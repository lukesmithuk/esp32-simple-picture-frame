# ESP32 E-Paper Picture Frame

A self-updating e-paper picture frame built on the [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/esp32-s3-photopainter.htm) board. Wakes on a schedule, fetches a photo from a WiFi server (or SD card), dithers it to the 6-colour E Ink Spectra 6 palette, displays it, and sleeps until next time.

Includes a self-hosted photo server with a web UI for uploading and managing images.

## Features

- **WiFi photo retrieval** from a self-hosted server with SD card fallback
- **Multi-frame support** — name frames, assign different images to each, set per-frame wake intervals
- **Floyd-Steinberg dithering** with measured palette values for accurate colour reproduction
- **Dynamic range compression** optimised for e-paper's limited tonal range
- **NTP time sync** updates the RTC on each WiFi connection
- **Image shuffle** cycles through all photos before repeating
- **Remote monitoring** — battery status, firmware version, and logs viewable in the web UI
- **Configurable wake interval** from the web UI or SD card config file, per-frame or global
- **Deep sleep** between updates for ultra-low power consumption

## Hardware

- [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/esp32-s3-photopainter.htm) — ESP32-S3, 7.3" e-paper, AXP2101 PMIC, PCF85063 RTC
- 7.3" E Ink Spectra 6 (E6) display — 800x480, 6 colours (Black, White, Yellow, Red, Blue, Green)
- SD card for config, logs, and fallback images
- Optional Li-Ion battery

## Quick Start

### Server (Raspberry Pi or any Linux machine)

**Option A — Docker (recommended).** Runs the multi-arch image published by CI
(`linux/amd64`, `linux/arm64`, `linux/arm/v7` — covers the Pi Zero 2W on 32-bit OS).
Needs a recent Docker Engine + Compose v2 (install via [get.docker.com](https://get.docker.com);
the `env_file` form in `compose.yaml` requires Compose **v2.24+**).

```bash
cd server
cp .env.example .env          # set PHOTOFRAME_API_KEY
docker compose up -d          # ghcr.io/lukesmithuk/esp32-simple-picture-frame
```

Photos, thumbnails, and the SQLite DB persist in `server/data/` (mounted to `/data`).
Open `http://<server-ip>:8080` to upload photos and monitor frames.

**Starts on boot** via compose's `restart: unless-stopped` — once you've run
`docker compose up -d` and Docker's daemon is enabled at boot
(`sudo systemctl is-enabled docker`; enable with `sudo systemctl enable docker`),
the container comes back after a reboot. No extra systemd unit needed.

**Migrating from an existing tarball install:**
```bash
cd server
./migrate-to-docker.sh /path/to/old/photoframe-server   # stops+disables old service, copies DB + images + thumbs
docker compose up -d
./uninstall.sh                                           # optional: remove the now-disabled old systemd unit
```

**Option B — Tarball + systemd (deprecated).**

> **Deprecated** in favour of Docker (above); kept for non-Docker hosts and
> slated for future removal.

```bash
curl -L https://github.com/lukesmithuk/esp32-simple-picture-frame/releases/latest/download/photoframe-server.tar.gz | tar xz
cd photoframe-server
./setup.sh    # creates venv, installs deps, prompts for API key, installs systemd service
```

For development from source: `./install.sh` then `PHOTOFRAME_API_KEY=yourkey ./run.sh`.

**Uninstall (either path):** `./uninstall.sh` (removes the systemd service; keep or delete data when prompted).

### Server development on Windows

Primary path is Docker Desktop (`cd server && docker compose up`). Without Docker:

```powershell
cd server
py -m venv venv
venv\Scripts\python -m pip install -r requirements.txt
venv\Scripts\python -m pytest                 # run tests
$env:PHOTOFRAME_API_KEY="changeme"; venv\Scripts\python main.py   # run locally
```

The `*.sh` scripts (`setup.sh`, `run.sh`, `uninstall.sh`, `migrate-to-docker.sh`) target Linux.

### Frame Firmware

Requires [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/).

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
idf.py build
python3 flash.py --timeout 30
```

### SD Card Setup

See `sdcard/config.txt.example` for a full reference. Minimum setup:

1. Copy `sdcard/config.txt.example` to your SD card as `config.txt` and edit it
2. Create an `images/` directory on the SD card with at least one baseline JPEG as a fallback (used when WiFi or the server is unavailable)

```
/sdcard/
  config.txt
  images/
    fallback.jpg      ← at least one image recommended
```

## Configuration

All config is via `/sdcard/config.txt` (key=value format, `#` comments).

| Key | Default | Description |
|-----|---------|-------------|
| `wifi_ssid` | *(none)* | WiFi network name. If absent, WiFi is skipped. |
| `wifi_password` | `""` | WiFi password |
| `server_url` | `""` | Photo server URL (e.g. `http://192.168.1.50:8080`) |
| `server_api_key` | `""` | Shared API key for server authentication |
| `wake_interval_hours` | 1 | Hours between image updates |
| `wake_interval_minutes` | 0 | Minutes between image updates |
| `wake_interval_seconds` | 0 | Seconds between image updates |
| `log_max_size_kb` | 256 | Max log file size before rotation |

The wake interval can also be set from the server's web UI — the server-provided value takes priority.

## Architecture

```
Frame (ESP32-S3)                    Server (Raspberry Pi)
┌──────────────────┐               ┌──────────────────────┐
│ Wake from sleep   │               │ FastAPI + SQLite      │
│ Connect WiFi      │──────────────▶│ GET /api/next         │
│ Sync NTP → RTC    │               │  (shuffle, no repeat) │
│ Upload logs       │──────────────▶│ POST /api/logs        │
│ Push status       │──────────────▶│ POST /api/status      │
│ Fetch image       │◀──────────────│ JPEG (800x480)        │
│ Decode + dither   │               │                       │
│ Display on e-paper │               │ Web UI (:8080)        │
│ Deep sleep        │               │  Upload / manage      │
└──────────────────┘               │  Frame dashboard      │
                                    └──────────────────────┘
```

If WiFi is unavailable, the frame falls back to images on the SD card.

## Project Structure

```
main/                    App entry point
components/
  board/                 I2C, PMIC (AXP2101), RTC (PCF85063)
  epd/                   SPI e-paper driver (Spectra 6)
  sdcard/                4-bit SDIO mount/unmount
  image_picker/          Image selection with shuffle history
  image_loader/          File → PSRAM buffer
  image_decode/          JPEG → scale → CDR → dither → 4bpp
  epd_text/              Bitmap font for error messages
  applog/                ESP_LOG tee to SD card file
  config/                Key=value config file reader
  wifi_fetch/            WiFi, NTP, HTTP client
server/                  Python photo server
  main.py                FastAPI app
  database.py            SQLite (images, frames, history)
  templates/             Web UI (Jinja2)
  tests/                 pytest (database + API)
```

## License

This project is for personal use. See individual component licenses where applicable.
