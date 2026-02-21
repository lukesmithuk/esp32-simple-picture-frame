#!/usr/bin/env python3
"""
Serial monitor for ESP32 bring-up.
Attaches to the running chip and prints output until Ctrl-C or --timeout seconds.
Does NOT reset the chip by default — pass --reset to trigger an RTS reset first.

Usage:
    python3 monitor.py [--port /dev/ttyACM0] [--baud 115200] [--timeout 60] [--reset]

WARNING: --reset uses a bare RTS toggle which can leave the I2C bus in a bad state
on this board.  Prefer 'python3 flash.py' (esptool reset + immediate attach) when
you need a clean boot capture.
"""

import argparse
import serial
import time
import sys
import signal

def main():
    """Entry point: open serial port, optionally reset the chip, print output.

    Reset sequence (only when --reset is given):
      - Assert RTS (EN pin low) for 100 ms, then deassert — this triggers the
        ESP32-S3 reset.  DTR is held low throughout to avoid spurious boot-mode
        entry.  A 2-second settle delay follows before reading begins.
      - WARNING: this bare RTS toggle can leave the I2C bus in a bad state on
        this board.  Use 'python3 flash.py' for a clean boot capture instead.

    Output loop:
      - Reads up to 4096 bytes per iteration, writes raw bytes to stdout so
        ANSI escape codes (IDF log colours) are preserved.
      - Exits on Ctrl-C or when --timeout seconds have elapsed.
      - A timeout of 0 (default) runs until Ctrl-C.

    Typical usage::

        python3 monitor.py                   # attach without resetting (default)
        python3 monitor.py --timeout 30      # capture 30 s of output
        python3 monitor.py --reset           # reset chip first (see WARNING above)
    """
    parser = argparse.ArgumentParser(description="ESP32 serial monitor")
    parser.add_argument("--port",    default="/dev/ttyACM0")
    parser.add_argument("--baud",    type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=0,
                        help="Stop after N seconds (0 = run until Ctrl-C)")
    parser.add_argument("--reset", action="store_true",
                        help="Reset chip via RTS before monitoring (see WARNING in module docstring)")
    args = parser.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.05)

    if args.reset:
        # Toggle RTS to reset the ESP32
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        print(f"[monitor] Reset chip on {args.port}, waiting for boot...",
              file=sys.stderr, flush=True)
        time.sleep(2)

    deadline = time.time() + args.timeout if args.timeout > 0 else None

    try:
        while True:
            if deadline and time.time() > deadline:
                print("\n[monitor] Timeout reached.", file=sys.stderr)
                break
            chunk = s.read(4096)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        print("\n[monitor] Ctrl-C, exiting.", file=sys.stderr)
    finally:
        s.close()

if __name__ == "__main__":
    main()
