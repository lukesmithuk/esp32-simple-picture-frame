#!/usr/bin/env python3
"""
Serial monitor for ESP32 bring-up.
Resets the chip via DTR/RTS, then prints output until Ctrl-C or --timeout seconds.

Usage:
    python3 monitor.py [--port /dev/ttyACM0] [--baud 115200] [--timeout 60]
"""

import argparse
import serial
import time
import sys
import signal

def main():
    parser = argparse.ArgumentParser(description="ESP32 serial monitor")
    parser.add_argument("--port",    default="/dev/ttyACM0")
    parser.add_argument("--baud",    type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=0,
                        help="Stop after N seconds (0 = run until Ctrl-C)")
    parser.add_argument("--no-reset", action="store_true",
                        help="Don't reset chip on connect")
    args = parser.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.05)

    if not args.no_reset:
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
