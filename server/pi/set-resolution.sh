#!/bin/bash
# set-resolution.sh - force the HDMI console resolution via the kernel command line.
#
# Usage (on the Pi):
#   sudo bash /boot/firmware/set-resolution.sh 720     # 1280x720 @ 60Hz
#   sudo bash /boot/firmware/set-resolution.sh 1080    # 1920x1080 @ 60Hz
#   sudo bash /boot/firmware/set-resolution.sh auto    # remove override (use monitor's preferred mode)
# Then reboot. A backup of the previous file is kept as cmdline.txt.bak.
set -euo pipefail

case "${1:-}" in
    720)  MODE="1280x720@60" ;;
    1080) MODE="1920x1080@60" ;;
    auto) MODE="" ;;
    *)
        echo "Usage: sudo bash $0 720|1080|auto"
        exit 1
        ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo:  sudo bash $0 $1"
    exit 1
fi

CMDLINE=""
for f in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    if [ -f "$f" ]; then CMDLINE="$f"; break; fi
done
if [ -z "$CMDLINE" ]; then
    echo "Error: cmdline.txt not found in /boot/firmware or /boot."
    exit 1
fi

cp "$CMDLINE" "$CMDLINE.bak"

# cmdline.txt must be a single line. Join any lines, drop CRs (in case it was
# edited on Windows), remove any existing HDMI video= override, tidy spaces.
LINE="$(tr '\r\n' '  ' < "$CMDLINE" \
    | sed -E 's/(^| )video=HDMI-A-[0-9]+:[^ ]*//g; s/  +/ /g; s/^ //; s/ $//')"

if [ -z "$LINE" ]; then
    echo "Error: cmdline.txt came out empty - leaving the original untouched."
    exit 1
fi

if [ -n "$MODE" ]; then
    LINE="$LINE video=HDMI-A-1:$MODE"
fi

printf '%s\n' "$LINE" > "$CMDLINE"

echo "Updated $CMDLINE (backup: $CMDLINE.bak):"
cat "$CMDLINE"
echo
if [ -n "$MODE" ]; then
    echo "Resolution set to $MODE. Reboot to apply:  sudo reboot"
else
    echo "Resolution override removed. Reboot to apply:  sudo reboot"
fi
