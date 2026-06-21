#!/bin/bash
# migrate-to-docker.sh — copy an existing tarball/systemd install's data
# (DB + images + thumbs) into the compose data volume.
#
# Usage: ./migrate-to-docker.sh /path/to/old/install [--force]
set -e

SRC="${1:-}"
FORCE="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="${PHOTOFRAME_DEST:-$SCRIPT_DIR/data}"
SERVICE_NAME="photoframe-server"

if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
    echo "Usage: $0 /path/to/old/install [--force]"
    echo "  (the dir containing photoframe.db, images/, thumbs/)"
    exit 1
fi
if [ ! -f "$SRC/photoframe.db" ]; then
    echo "Error: $SRC/photoframe.db not found — is this a server install dir?"
    exit 1
fi

# Stop + disable the old service so SQLite is not mid-write during the copy and
# the service does not resurrect on reboot to fight the container for port 8080.
# The unit file itself is left in place — run ./uninstall.sh to remove it fully.
if command -v systemctl >/dev/null 2>&1 \
   && systemctl list-unit-files 2>/dev/null | grep -q "${SERVICE_NAME}.service"; then
    echo "Stopping and disabling ${SERVICE_NAME} service..."
    sudo systemctl stop "${SERVICE_NAME}" || true
    sudo systemctl disable "${SERVICE_NAME}" || true
    echo "  (unit file left in place — run ./uninstall.sh to remove it fully)"
fi

# Refuse to clobber a non-empty destination unless --force.
if [ -d "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ] && [ "$FORCE" != "--force" ]; then
    echo "Error: $DEST is not empty. Re-run with --force to overwrite."
    exit 1
fi

echo "Migrating data from $SRC -> $DEST"
mkdir -p "$DEST"
cp "$SRC/photoframe.db" "$DEST/photoframe.db"
# Copy directory *contents* (src/images/.) into dest so a --force re-run merges
# in place instead of nesting under an existing dest/images/.
if [ -d "$SRC/images" ]; then
    mkdir -p "$DEST/images"
    cp -r "$SRC/images/." "$DEST/images/"
fi
if [ -d "$SRC/thumbs" ]; then
    mkdir -p "$DEST/thumbs"
    cp -r "$SRC/thumbs/." "$DEST/thumbs/"
fi

echo "Done. Next:"
echo "  cd $SCRIPT_DIR && docker compose up -d"
