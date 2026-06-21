# Migrating from the tarball + systemd install to Docker

This guide moves an existing **tarball + systemd** photo-frame server to the
**containerised** deployment, preserving your photos, thumbnails, and database.
Run every step **on the Pi** (or wherever the old server runs) — that is where
the old install, Docker, and the data live.

> The container is the recommended deployment; the tarball/systemd path is
> deprecated. See the [README](../README.md) for the plain (non-migration)
> Docker setup.

## What the migration does — and doesn't

`migrate-to-docker.sh /path/to/old/install`:

- **Stops and disables** the old `photoframe-server.service` (via `sudo
  systemctl`) so it can't hold port 8080 or restart on reboot. The unit file is
  left in place — remove it later with `uninstall.sh` (optional).
- Copies **`photoframe.db`, `images/`, and `thumbs/`** into `server/data/` (the
  Compose volume), merging in place so a `--force` re-run is safe.
- **Refuses** to overwrite a non-empty `data/` unless you pass `--force`.
- **Does NOT copy `server.env`** — so your **API key is not migrated
  automatically**. You must set it yourself (Step 2). This is the easiest thing
  to get wrong: if the key changes, the frame fails to authenticate.

Per-frame settings, wake intervals, image assignments, and logs all live in
`photoframe.db`, so they migrate with the database copy.

## Prerequisites

```bash
docker compose version                 # need Compose v2.24+
sudo systemctl enable --now docker     # Docker's own daemon must be enabled so the
                                       # container restarts on boot (this REPLACES the
                                       # old per-app systemd unit)
```

## Procedure

```bash
# 1. Get compose.yaml + the migrate script onto the Pi. The image itself is
#    pulled from GHCR — no local build needed.
git clone https://github.com/lukesmithuk/esp32-simple-picture-frame.git
cd esp32-simple-picture-frame/server

# 2. Reuse the EXISTING API key (the frame's SD-card `server_api_key` must keep
#    matching it). Read it from the old install's server.env:
cp .env.example .env
grep PHOTOFRAME_API_KEY /path/to/old/photoframe-server/server.env
nano .env                              # paste the key into PHOTOFRAME_API_KEY=

# 3. Migrate: stops+disables the old service, then copies DB + images + thumbs.
./migrate-to-docker.sh /path/to/old/photoframe-server
#   Watch the output: confirm "Stopping and disabling photoframe-server service..."
#   actually succeeded (it uses sudo). If it didn't, the old service may still
#   hold port 8080 and Step 4 will fail to bind.

# 4. Start the container.
docker compose up -d

# 5. Verify.
curl -fsS http://localhost:8080/healthz        # -> {"status":"ok"}
docker compose logs -f                          # replaces: journalctl -u photoframe-server -f
```

Open `http://<pi-ip>:8080` and confirm your gallery, frames, and settings are
all present.

## systemd considerations

| Concern | What to do |
|---------|-----------|
| **Port 8080 conflict** | The old service and the container both bind 8080. Step 3 stops+disables the old one first — don't `docker compose up` before migrating. You need `sudo` rights for the stop/disable to take effect. |
| **DB integrity** | Stopping the service *before* the copy (Step 3 does this) avoids copying a half-written SQLite WAL. Never copy `photoframe.db` while the old service is running. |
| **Leftover unit file** | The migration disables but keeps `/etc/systemd/system/photoframe-server.service`. Removal is optional (a disabled unit won't run). Do it only once you're sure you won't roll back (see below), then: `sudo rm /etc/systemd/system/photoframe-server.service && sudo systemctl daemon-reload`. (The repo's `server/uninstall.sh` does the same; its "Delete server data?" prompt targets the *legacy* `images/`, `thumbs/`, `photoframe.db`, `venv/` in its own dir — **not** `server/data/` — but answer `N` to keep your old install as a rollback.) |
| **Management commands change** | There is no `photoframe-server.service` anymore. Use `docker compose ps`, `docker compose logs -f`, and `docker compose restart` instead of `systemctl status` / `journalctl -u`. |

## Rollback

The migration **copies** (never moves), so the old venv, data, and unit file
stay intact. As long as you have **not** removed the unit file (see the systemd
table above), revert to the systemd install with:

```bash
docker compose down
sudo systemctl enable --now photoframe-server
```

If you already removed the unit file, reinstall it from the old tarball with its
`install-service.sh` (or `setup.sh`) before re-enabling.

## Troubleshooting

- **Frame shows an auth error after migrating** — the API key in `.env` doesn't
  match the frame's `server_api_key`. Re-check Step 2.
- **`docker compose up` fails to bind 8080** — the old service is still running.
  `sudo systemctl stop photoframe-server` and retry.
- **Gallery is empty after `--force`** — you are on an old build; the current
  script merges contents in place. Ensure you cloned the latest `server/`.
- **`docker compose` rejects `compose.yaml`** — your Compose is older than v2.24.
  Install a current Docker via <https://get.docker.com>.
