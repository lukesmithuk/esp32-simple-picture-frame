# WiFi Photo Retrieval — Design Spec

## Overview

Add WiFi support to the ESP32 e-paper picture frame so it can retrieve photos from a self-hosted server instead of (or in addition to) the SD card. Includes a server with a web UI for photo management, frame status reporting, log upload, and NTP time sync.

## Goals

- Frame fetches the next image from a server over WiFi
- Falls back to SD card if WiFi or server is unavailable
- Server provides a web UI for uploading and managing photos
- Frame pushes status (battery, SD, firmware) and logs to server each wake cycle
- Frame syncs RTC via NTP when WiFi is available
- SD card remains required (config, logs, fallback images)

## Non-Goals (v1)

- Server-side image processing (resize, progressive→baseline conversion)
- HTTPS / encrypted transport (TODO: add later)
- Provisioning UI (BLE, captive portal) — config is manual via `config.txt`
- Node.js server rewrite (future learning project)

---

## Server

### Tech Stack

- Python 3, FastAPI, SQLite
- Deployed on Raspberry Pi (Zero or Pi 5)
- Jinja2 templates for web UI

### API Endpoints

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `/api/next` | GET | API key | Return next image as JPEG. Server tracks per-frame show history, shuffles without repeat. |
| `/api/status` | POST | API key | Frame pushes status: battery %, voltage, charging state, USB connected, SD free space, firmware version. |
| `/api/logs` | POST | API key | Frame pushes new log lines since last upload. Body is raw text. |
| `/api/time` | GET | API key | Returns server UTC time (fallback if NTP fails). |
| `/` | GET | None | Web UI: gallery, upload, frame status dashboard. |

### Authentication

API key sent in `X-API-Key` header. Plaintext over HTTP for v1.

**TODO:** Add HTTPS support. The ESP32-S3 supports TLS via `esp_tls`. Would require a certificate on the Pi (self-signed or Let's Encrypt). Low risk on a local network for v1 since the API key only grants access to photos.

### Storage

- **Images:** files in a directory on the Pi filesystem.
- **Database (SQLite):**
  - `images` table: id, filename, uploaded_at
  - `frames` table: api_key, name, last_seen, battery_percent, battery_mv, charging, usb_connected, sd_free_kb, firmware_version
  - `history` table: frame_id, image_id, shown_at

### Shuffle Logic

Server-side, per-frame. When `/api/next` is called:

1. Get all image IDs from the `images` table.
2. Get this frame's show history from `history`.
3. Filter out images already shown.
4. If no candidates remain, clear this frame's history (full cycle complete).
5. Pick randomly from candidates.
6. Record in `history`, return the image bytes.

Handles added/removed images: history entries for deleted images are ignored; new images appear as candidates immediately.

### Web UI

- Gallery grid showing uploaded images with thumbnails.
- Upload form (file picker, multi-file).
- Delete button per image.
- Frame status panel: last seen timestamp, battery level, charging state, last log excerpt.
- Simple, functional — no framework, plain HTML/CSS/JS.

---

## Frame Firmware

### New Component: `wifi_fetch`

Owns WiFi lifecycle and HTTP communication with the server.

**Public API:**

```c
// Connect to WiFi, sync time via NTP, update RTC.
esp_err_t wifi_fetch_init(const char *ssid, const char *password);

// GET /api/next — download next image into a PSRAM buffer.
// Same contract as image_loader_load(): caller must free *out_buf.
esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size);

// POST /api/status — push frame status to server.
esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key);

// POST /api/logs — push new log lines since last upload.
// Tracks upload offset in offset_path. Caps at 16 KB per cycle.
esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path);

// Disconnect WiFi, release resources.
void wifi_fetch_deinit(void);
```

**NTP sync:** Performed inside `wifi_fetch_init()` after WiFi connects. Uses `esp_sntp` to get UTC, then writes to RTC via `board_rtc_set_time()`. If NTP fails within a timeout (e.g. 5s), continues without sync.

**Dependencies:** `esp_wifi`, `esp_http_client`, `esp_netif`, `esp_hw_support`, `board`, `config`.

### Modified `app_main` Flow

```
boot → applog_init → board_init → epd_init → mount SD → applog_start → config_load

→ if wifi_ssid is set in config:
    wifi_fetch_init(ssid, password)        # WiFi connect + NTP → RTC sync
    wifi_fetch_post_status(...)            # push battery/SD state
    wifi_fetch_post_logs(...)              # push new log lines since last upload
    ret = wifi_fetch_image(..., &img_buf)  # fetch next image
    wifi_fetch_deinit()                    # disconnect WiFi to save power
    if ret != ESP_OK:
        fall through to SD card path

→ if no img_buf yet:
    image_picker_pick → image_loader_load  # existing SD card fallback

→ image_decode_jpeg → epd_display → cleanup → sleep
```

### Log Rolling

Added to `applog_start()`: before opening the log file, check its size. If it exceeds a configurable threshold, rename `system.log` to `system.log.1` (overwriting any previous backup) and start fresh.

- Only one backup file kept (`.1`).
- Threshold from config: `log_max_size_kb` (default: 256).

### Log Upload

Incremental upload using a stored byte offset:

1. Read offset from `/sdcard/.log_offset` (default 0 if missing).
2. If `system.log` size < stored offset (file was rotated), reset offset to 0.
3. Open `system.log`, seek to offset, read up to 16 KB.
4. POST to `/api/logs`.
5. On success, write new offset to `.log_offset`.
6. On failure, skip — next cycle retries from the same position.

### Status Push

`wifi_fetch_post_status()` collects and sends:

- Battery: connected, percent, voltage (mV), charging state
- USB: connected
- SD card: free space (KB)
- Firmware: version string (from build)
- Uptime: milliseconds since boot

Sent as JSON POST body to `/api/status`.

### Config Keys

Added to existing `/sdcard/config.txt`:

```
# WiFi (all optional — if wifi_ssid is absent, WiFi is skipped)
wifi_ssid=MyNetwork
wifi_password=secret123
server_url=http://192.168.1.50:8080
server_api_key=abc123

# Log rolling
log_max_size_kb=256
```

---

## Fallback Behaviour

| Condition | Behaviour |
|-----------|-----------|
| No `wifi_ssid` in config | WiFi skipped, SD card only (current behaviour) |
| WiFi connect fails | Log warning, fall back to SD card |
| Server unreachable | Log warning, fall back to SD card |
| `/api/next` returns no image (gallery empty) | Fall back to SD card |
| SD card also has no images | Display "No images found" error on EPD |
| Status/log push fails | Log warning, continue (non-fatal) |
| NTP sync fails | Continue with existing RTC time |

---

## Future Improvements

- HTTPS transport (TLS on ESP32-S3 + certificate on Pi)
- Server-side image processing (resize, baseline JPEG conversion, dither preview)
- WiFi provisioning (BLE setup or captive portal)
- Node.js server rewrite
- Multiple frame support in web UI (per-frame galleries, scheduling)
- OTA firmware updates via server
