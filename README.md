# esp32-simple-picture-frame
Simple waveshare esp32 picture frame

## Building

Source the ESP-IDF environment, then build:

```bash
source /home/pls/espressif/release-v6.0/esp-idf/export.sh
idf.py build
```

The first build takes a few minutes. Subsequent builds are incremental.

## Flashing

Connect the board via USB-C, then flash:

```bash
idf.py -p /dev/ttyACM0 flash
```

If the device is not detected, enter download mode by holding the **BOOT** button and pressing **PWR**, then retry.

## Monitoring serial output

Start the serial monitor after flashing:

```bash
idf.py -p /dev/ttyACM0 monitor
```

Exit the monitor with **Ctrl+]**.

## Flash and monitor in one step

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

## Capturing output to a file

Pipe the monitor output through `tee` to capture while still viewing it live:

```bash
idf.py -p /dev/ttyACM0 monitor 2>&1 | tee debug.log
```

Exit with **Ctrl+]** as usual. The full session is saved in `debug.log`.
