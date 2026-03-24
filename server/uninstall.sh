#!/bin/bash
# uninstall.sh — remove the Photo Frame Server systemd service
set -e

SERVICE_NAME="photoframe-server"

echo "=== Photo Frame Server Uninstall ==="

if systemctl list-unit-files | grep -q "${SERVICE_NAME}.service"; then
    echo "Stopping and removing systemd service..."
    sudo systemctl stop ${SERVICE_NAME} 2>/dev/null || true
    sudo systemctl disable ${SERVICE_NAME} 2>/dev/null || true
    sudo rm -f /etc/systemd/system/${SERVICE_NAME}.service
    sudo systemctl daemon-reload
    echo "Service removed."
else
    echo "No systemd service found."
fi

read -p "Delete server data (images, database, config)? [y/N]: " DELETE_DATA
if [ "$DELETE_DATA" = "y" ] || [ "$DELETE_DATA" = "Y" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    rm -rf "$SCRIPT_DIR/venv" "$SCRIPT_DIR/images" "$SCRIPT_DIR/thumbs"
    rm -f "$SCRIPT_DIR/photoframe.db" "$SCRIPT_DIR/server.env"
    echo "Data deleted."
else
    echo "Data preserved."
fi

echo "Done."
