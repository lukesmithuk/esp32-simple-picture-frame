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
- Server-side image validation (baseline JPEG check, size limits)

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
| `/api/status` | POST | API key | Frame pushes status as JSON body. |
| `/api/logs` | POST | API key | Frame pushes new log lines since last upload. Body is raw text. |
| `/` | GET | None | Web UI: gallery, upload, frame status dashboard. |

#### `/api/next` Response Contract

- **200 OK**: `Content-Type: image/jpeg`, body is raw JPEG bytes, `X-Image-Name` header with filename for logging.
- **204 No Content**: gallery is empty, no images available. Frame falls back to SD card.
- **401 Unauthorized**: bad or missing API key.
- **500**: server error.

The server enforces a 4 MB max image size (matching `IMAGE_LOADER_MAX_FILE_BYTES`). `Content-Length` header is always set (no chunked transfer) so the frame can pre-allocate a PSRAM buffer.

### Authentication

API key sent in `X-API-Key` header. Plaintext over HTTP for v1.

**TODO:** Add HTTPS support. The ESP32-S3 supports TLS via `esp_tls`. Would require a certificate on the Pi (self-signed or Let's Encrypt). Low risk on a local network for v1 since the API key only grants access to photos.

### Frame Identification

Each frame is identified by its ESP32 MAC address (sent in `X-Frame-ID` header alongside the API key). The API key handles auth; the MAC address handles per-frame tracking. This allows multiple frames to share an API key while maintaining independent show history and status.

### Storage

- **Images:** files in a directory on the Pi filesystem.
- **Database (SQLite):**
  - `images` table: id, filename, uploaded_at
  - `frames` table: id, mac_address, api_key, name, last_seen, battery_percent, battery_mv, charging, usb_connected, sd_free_kb, firmware_version
  - `history` table: frame_id, image_id, shown_at
  - Schema versioning deferred — v1 uses `CREATE TABLE IF NOT EXISTS`.

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

### Memory Budget

WiFi on ESP32-S3 uses ~37 KB internal DRAM for buffers plus the lwIP stack. Enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` in sdkconfig.defaults to push WiFi/lwIP allocations to PSRAM where possible.

Peak memory during `wifi_fetch_image()`: WiFi stack active + up to 4 MB JPEG in PSRAM + 192 KB frame buffer in PSRAM. This fits within 8 MB PSRAM with internal DRAM relieved by the SPIRAM WiFi setting.

WiFi is disconnected (`wifi_fetch_deinit()`) before image decode begins, freeing WiFi memory before the decode pipeline's own PSRAM allocations (RGB buffers, error buffers).

### New Component: `wifi_fetch`

Owns WiFi lifecycle and HTTP communication with the server.

**Public API:**

```c
// Connect to WiFi, sync time via NTP, update RTC.
// Timeout: 10 seconds for WiFi association, 5 seconds for NTP.
// Returns ESP_ERR_TIMEOUT if WiFi connect fails.
esp_err_t wifi_fetch_init(const char *ssid, const char *password);

// GET /api/next — download next image into a PSRAM buffer.
// Same contract as image_loader_load(): caller must free *out_buf.
// Requires Content-Length from server; pre-allocates buffer.
// Returns ESP_ERR_NOT_FOUND for 204 (no images), ESP_FAIL for errors.
esp_err_t wifi_fetch_image(const char *server_url, const char *api_key,
                           uint8_t **out_buf, size_t *out_size);

// POST /api/status — push frame status to server.
// Caller passes a status struct; wifi_fetch is purely transport.
esp_err_t wifi_fetch_post_status(const char *server_url, const char *api_key,
                                 const wifi_fetch_status_t *status);

// POST /api/logs — push new log lines since last upload.
// Flushes applog before reading. Tracks upload offset in offset_path.
// Caps at 16 KB per cycle.
esp_err_t wifi_fetch_post_logs(const char *server_url, const char *api_key,
                               const char *log_path, const char *offset_path);

// Disconnect WiFi, release resources.
void wifi_fetch_deinit(void);
```

**Status struct** (passed by caller, keeps wifi_fetch as pure transport):

```c
typedef struct {
    bool  battery_connected;
    int   battery_percent;
    int   battery_mv;
    bool  charging;
    bool  usb_connected;
    int   sd_free_kb;
    const char *firmware_version;
} wifi_fetch_status_t;
```

**NTP sync:** Performed inside `wifi_fetch_init()` after WiFi connects. Uses `esp_sntp` to get UTC, then writes to RTC via `board_rtc_set_time()`. If NTP fails within 5 seconds, continues without sync.

**WiFi timeout:** `wifi_fetch_init()` waits up to 10 seconds for WiFi association. Returns `ESP_ERR_TIMEOUT` on failure. No retries — the frame falls back to SD card.

**Dependencies:** `esp_wifi`, `esp_http_client`, `esp_netif`, `esp_hw_support`, `board`.

### Modified `app_main` Flow

```
boot → applog_init → board_init → epd_init → mount SD → config_load

→ if wifi_ssid is set in config:
    wifi_fetch_init(ssid, password)        # WiFi connect + NTP → RTC sync
    wifi_fetch_post_logs(...)              # push new log lines FIRST (before rolling)
    applog_start(SYSTEM_LOG)               # opens log file (with rolling check)
    wifi_fetch_post_status(...)            # push battery/SD state
    ret = wifi_fetch_image(..., &img_buf)  # fetch next image
    wifi_fetch_deinit()                    # disconnect WiFi to save power
    if ret != ESP_OK:
        fall through to SD card path
→ else:
    applog_start(SYSTEM_LOG)               # no WiFi — just start logging

→ if no img_buf yet:
    image_picker_pick → image_loader_load  # existing SD card fallback

→ image_decode_jpeg → epd_display → cleanup → sleep
```

Note: `applog_start()` (which includes log rolling) happens *after* log upload to prevent losing unuploaded lines during rotation.

### Log Rolling

Added to `applog_start()`: before opening the log file, check its size. If it exceeds a configurable threshold, rename `system.log` to `system.log.1` (overwriting any previous backup) and start fresh.

- Only one backup file kept (`.1`).
- Threshold from config: `log_max_size_kb` (default: 256).
- Rolling happens after log upload (see flow above) to avoid losing unuploaded content.

### Log Upload

Incremental upload using a stored byte offset:

1. Flush applog buffer (if buffering) to ensure file is current.
2. Read offset from `/sdcard/.log_offset` (default 0 if missing).
3. If `system.log` size < stored offset (file was rotated), reset offset to 0.
4. Open `system.log`, seek to offset, read up to 16 KB.
5. POST to `/api/logs`.
6. On success, write new offset to `.log_offset`.
7. On failure, skip — next cycle retries from the same position.

### Status Push

Caller (main.c) builds a `wifi_fetch_status_t` struct from `board_*` APIs, passes it to `wifi_fetch_post_status()`. The component serialises it as JSON and POSTs to `/api/status`. This keeps `wifi_fetch` decoupled from `board` internals.

Status includes:
- Battery: connected, percent, voltage (mV), charging state
- USB: connected
- SD card: free space (KB)
- Firmware: version string (compile-time `#define`)

### Sensitive Config Keys

The `config` component logs all key=value pairs at INFO level. WiFi passwords and API keys must not appear in logs. The config component will be updated to mask values for keys containing `password` or `key` (e.g. log as `wifi_password = ****`).

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
| WiFi connect times out (10s) | Log warning, fall back to SD card |
| Server unreachable | Log warning, fall back to SD card |
| `/api/next` returns 204 (gallery empty) | Fall back to SD card |
| Image fetch fails mid-download | Free partial buffer, fall back to SD card |
| SD card also has no images | Display "No images found" error on EPD |
| Status/log push fails | Log warning, continue (non-fatal) |
| NTP sync fails | Continue with existing RTC time |

---

## Future Improvements

- HTTPS transport (TLS on ESP32-S3 + certificate on Pi)
- Server-side image processing (resize, baseline JPEG conversion, dither preview)
- Server-side image validation (reject non-baseline JPEG, enforce size limits)
- WiFi provisioning (BLE setup or captive portal)
- Node.js server rewrite
- Multiple frame support in web UI (per-frame galleries, scheduling)
- OTA firmware updates via server
- Schema migration tooling for SQLite
