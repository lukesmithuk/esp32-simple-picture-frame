---
name: build-flash
description: Build and flash ESP32 firmware with proper IDF v5.5.3 environment setup
disable-model-invocation: true
---

# Build & Flash ESP32 Firmware

Build the ESP-IDF project and optionally flash it to the connected board. This skill ensures the IDF environment is correctly sourced before running any build commands.

## Arguments

- `build` (default if no args) — build only
- `flash` — build + flash + monitor
- `flash --no-monitor` — build + flash without monitor
- `flash --timeout N` — build + flash + monitor with timeout
- `clean` — full clean rebuild (removes build/ and sdkconfig)
- `menuconfig` — open menuconfig

## Instructions

**Every `idf.py` or flash command MUST be prefixed with the IDF environment activation:**

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
```

Run commands based on the argument:

### `build` (default)
```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py build
```

### `flash`
```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py build && python3 flash.py --timeout 30
```
Pass through any extra flags (e.g. `--no-monitor`, `--timeout N`) to `flash.py`.

### `clean`
```bash
rm -rf build sdkconfig && source ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py build
```
This is required when `sdkconfig.defaults` has changed.

### `menuconfig`
```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py menuconfig
```

## Notes

- The working directory must be the project root (`/home/pls/esp32-simple-picture-frame`)
- `flash.py` handles esptool invocation and uses `--before default-reset` so no BOOT button is needed
- Monitor runs via `monitor.py` (called by `flash.py` unless `--no-monitor`)
- If the build fails, check the error output — do NOT re-run without investigating
