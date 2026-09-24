#!/bin/bash
# update.sh — update a running Docker deployment to the latest published image.
#
# Pulls the new image first (so a network failure changes nothing), stops the
# container, backs up the SQLite DB, starts the new container, and waits for
# /healthz. Also the way to apply .env changes (compose recreates on change).
#
# Usage: ./update.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTAINER="photoframe-server"
DATA_DIR="$SCRIPT_DIR/data"
BACKUP_DIR="$DATA_DIR/backups"
KEEP_BACKUPS=5
HEALTH_TIMEOUT=90  # seconds

cd "$SCRIPT_DIR"

if ! docker compose version >/dev/null 2>&1; then
    echo "Error: 'docker compose' not available (needs Docker Engine + Compose v2)."
    exit 1
fi
if [ ! -f compose.yaml ]; then
    echo "Error: compose.yaml not found in $SCRIPT_DIR"
    exit 1
fi

OLD_IMAGE="$(docker inspect -f '{{.Image}}' "$CONTAINER" 2>/dev/null || true)"
IMAGE="$(docker compose config --images | head -n 1)"

# Plain `docker pull`, not `docker compose pull`: compose.yaml also has
# `build: .`, and compose treats a failed pull of a buildable service as a
# warning (exit 0). A failed pull must abort before anything is stopped.
echo "Pulling $IMAGE..."
docker pull "$IMAGE"

# Stop before copying so SQLite is not mid-write during the backup.
BACKUP=""
if [ -f "$DATA_DIR/photoframe.db" ]; then
    docker compose stop
    mkdir -p "$BACKUP_DIR"
    BACKUP="$BACKUP_DIR/photoframe-$(date +%Y%m%d-%H%M%S).db"
    cp "$DATA_DIR/photoframe.db" "$BACKUP"
    echo "Backed up database to $BACKUP"
    # Keep only the newest $KEEP_BACKUPS backups.
    ls -1t "$BACKUP_DIR"/photoframe-*.db 2>/dev/null | tail -n +$((KEEP_BACKUPS + 1)) \
        | while read -r old; do rm -f "$old"; done
fi

echo "Starting container..."
docker compose up -d

echo -n "Waiting for /healthz"
healthy=""
for _ in $(seq 1 $((HEALTH_TIMEOUT / 3))); do
    if docker compose exec -T photoframe-server python -c \
        "import urllib.request; urllib.request.urlopen('http://localhost:8080/healthz', timeout=5)" \
        >/dev/null 2>&1; then
        healthy=1
        break
    fi
    echo -n "."
    sleep 3
done
echo

if [ -z "$healthy" ]; then
    echo "Error: server did not become healthy within ${HEALTH_TIMEOUT}s. Recent logs:"
    docker compose logs --tail 40
    echo
    echo "To roll back:"
    [ -n "$BACKUP" ] && echo "  database backup: $BACKUP (copy over $DATA_DIR/photoframe.db while stopped)"
    [ -n "$OLD_IMAGE" ] && echo "  previous image:  $OLD_IMAGE"
    echo "  steps: docker compose stop; cp <backup> $DATA_DIR/photoframe.db;"
    echo "         docker tag <previous image> $IMAGE; docker compose up -d"
    exit 1
fi

NEW_IMAGE="$(docker inspect -f '{{.Image}}' "$CONTAINER" 2>/dev/null || true)"
if [ -n "$OLD_IMAGE" ] && [ "$OLD_IMAGE" = "$NEW_IMAGE" ]; then
    echo "Server healthy (image unchanged — already up to date)."
else
    echo "Server healthy on new image ${NEW_IMAGE:0:19}."
fi
echo "Recent logs:"
docker compose logs --tail 15
