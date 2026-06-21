# Containerised server deployment + CI + Windows dev — Design

**Date:** 2026-06-21
**Status:** Approved (pending spec review)

## Summary

Move the Python photo-frame server (`server/`) from a tarball + systemd
deployment to a containerised one, mirroring the patterns in
[`lukesmithuk/personal-briefing`](https://github.com/lukesmithuk/personal-briefing):
a `Dockerfile`, a `compose.yaml`, a multi-arch GitHub CI pipeline that publishes
to GHCR, and a smoke-tested release flow. Also ensure the **server** development
workflow works on the Windows 11 laptop, and provide a clean migration path from
an existing tarball install to the container.

The firmware (ESP-IDF) toolchain on Windows is **out of scope**.

## Decisions

| Question | Decision |
|----------|----------|
| Windows dev scope | **Server only** — firmware toolchain stays on Linux |
| Deploy target | Pi Zero 2W, **32-bit** Raspberry Pi OS (`uname -m` → `armv7l`) |
| Container Python | **3.14** (`python:3.14-slim`), matching the laptop + personal-briefing |
| Build platforms | `linux/amd64`, `linux/arm64`, `linux/arm/v7` |
| Remote access | **LAN-only** — no cloudflared tunnel |
| Old deploy path | **Keep** tarball/systemd, but mark **deprecated** (future removal) |
| CI build shape | **Parallel matrix** of runners + merge job (personal-briefing style) |

## Out of scope

- ESP-IDF firmware build/flash/monitor on Windows.
- Cloudflare tunnel / any internet-facing access.
- Removing the tarball/systemd path now (only deprecate it).

---

## Section 1 — Server code changes

Minimal changes in `server/`, each preserving current native-dev behaviour.

### 1.1 `/healthz` endpoint (`main.py`)
Add an unauthenticated `GET /healthz` returning `{"status": "ok"}` with HTTP 200.
Used by the Docker healthcheck and the CI smoke test. No DB access required.

### 1.2 Configurable data directory (`config.py`)
Introduce a data root so the SQLite DB, images, and thumbnails live on a
mountable volume in the container while defaulting to today's layout for native
dev:

```python
DATA_DIR = Path(os.environ.get("PHOTOFRAME_DATA_DIR", BASE_DIR))
IMAGES_DIR = DATA_DIR / "images"
THUMBS_DIR = DATA_DIR / "thumbs"
DB_PATH = DATA_DIR / "photoframe.db"
```

- Default (`PHOTOFRAME_DATA_DIR` unset) → `BASE_DIR`, i.e. **no behaviour change**
  for existing native/tarball installs.
- Container sets `PHOTOFRAME_DATA_DIR=/data` and mounts a volume there.
- `DATA_DIR` is created on import (alongside the existing `images/`/`thumbs/`
  `mkdir`).

### 1.3 Lint config (ruff)
Add a minimal `ruff` configuration (`server/ruff.toml` or a `[tool.ruff]` table
in a new `server/pyproject.toml` — implementer's choice, keep it minimal and not
a packaging migration). Fix any findings in the existing server code so
`ruff check` passes cleanly in CI.

---

## Section 2 — Container files (in `server/`)

### 2.1 `Dockerfile`
- Base: `python:3.14-slim`.
- `ENV PYTHONUNBUFFERED=1`.
- Install Pillow's **runtime** image libs (`libjpeg62-turbo`, `libwebp7`,
  `zlib1g`) **and** its **build** deps (`gcc`, `libjpeg-dev`, `zlib1g-dev`,
  `libwebp-dev`) in one layer; `pip install` using **piwheels as an extra
  index** (`--extra-index-url https://www.piwheels.org/simple`) so armv7 pulls a
  prebuilt Pillow wheel when one exists; **purge the build-only deps** in the
  same `RUN` layer to keep the image small.
  - Rationale: Python 3.14 is new — piwheels may lack a `cp314` armv7 wheel for
    Pillow 12.1.1. With build deps present, pip compiles from source under
    emulation as a fallback; with a wheel available the build is fast. amd64 and
    arm64 use PyPI/manylinux wheels and never compile.
- Install **runtime-only** requirements (strip `pytest`/`httpx`/`pytest-asyncio`,
  matching the existing `release.yml` filter).
- `COPY` `main.py`, `config.py`, `database.py`, `templates/`, `static/`.
- `EXPOSE 8080`; `CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8080"]`.

### 2.2 `compose.yaml`
Single service, LAN-only:

```yaml
services:
  photoframe-server:
    image: ghcr.io/lukesmithuk/esp32-simple-picture-frame:latest
    build: .
    container_name: photoframe-server
    restart: unless-stopped
    ports:
      - "8080:8080"
    env_file:
      - path: .env
        required: false
    environment:
      PHOTOFRAME_DATA_DIR: /data
    healthcheck:
      test: ["CMD", "python", "-c",
             "import urllib.request; urllib.request.urlopen('http://localhost:8080/healthz', timeout=5)"]
      interval: 60s
      timeout: 10s
      retries: 3
      start_period: 20s
    volumes:
      - ./data:/data
```

### 2.3 `.dockerignore`
Exclude `venv/`, `data/`, `__pycache__/`, `*.pyc`, `*.db`, `images/`, `thumbs/`,
`tests/`, `.pytest_cache/`.

### 2.4 `.env.example`
```
PHOTOFRAME_API_KEY=
PHOTOFRAME_PORT=8080
```

---

## Section 3 — CI pipeline (`.github/workflows/ci.yml`, new)

Triggers: `push` to `main`, and `pull_request`. Image:
`ghcr.io/lukesmithuk/esp32-simple-picture-frame`.

### 3.1 `test` job (`ubuntu-latest`)
- `actions/setup-python@v6` with `3.14`.
- `pip install -r server/requirements.txt`.
- `ruff check` (in `server/`).
- `pytest -q` (in `server/`).

### 3.2 `build` job — parallel matrix (`needs: test`)
Three legs, each on its own runner, building one platform and **smoke-testing it
by booting the freshly built image** and polling `/healthz`:

| `platform` | `runner` |
|------------|----------|
| `linux/amd64` | `ubuntu-latest` |
| `linux/arm64` | `ubuntu-24.04-arm` |
| `linux/arm/v7` | `ubuntu-24.04-arm` |

Per leg:
1. `setup-buildx-action`. (armv7 on the arm64 runner runs aarch32 near-natively;
   add `setup-qemu-action` defensively for binfmt.)
2. Build + `load: true` into the local daemon, tag `photoframe-smoke:latest`,
   with `cache-from`/`cache-to: type=gha` scoped per platform.
3. **Smoke:** `docker run -d -p 8080:8080 photoframe-smoke:latest`, then poll
   `http://localhost:8080/healthz` until 200 (with a timeout); tear down in an
   `always()` step.
4. On `push` only: log in to GHCR (`packages: write`, `GITHUB_TOKEN`) and
   re-build with `outputs=type=image,...,push-by-digest=true,push=true` (cache
   hit from step 2), then upload the digest as an artifact.

PRs run steps 1–3 (build + smoke, all three arches) and **do not push**.

### 3.3 `merge` job (`ubuntu-latest`, `needs: build`, `push` only)
Download the three digest artifacts and
`docker buildx imagetools create` a single multi-arch manifest tagged
`:latest` and `:${{ github.sha }}`; `imagetools inspect` to verify.

### 3.4 `release.yml`
Unchanged behaviour. Add a header comment marking the tarball release as
**deprecated** in favour of the GHCR image.

---

## Section 4 — Windows dev + docs

### 4.1 Windows server dev
- **Primary:** Docker Desktop — `cd server && docker compose up` (with a `.env`
  copied from `.env.example`).
- **Native fallback (no Docker):** documented `py` launcher commands:
  ```
  cd server
  py -m venv venv
  venv\Scripts\python -m pip install -r requirements.txt
  venv\Scripts\python -m pytest        # tests
  $env:PHOTOFRAME_API_KEY="changeme"; venv\Scripts\python main.py   # run
  ```
- The bash `run.sh`/`setup.sh`/`install.sh` stay as the Linux path; not rewritten.

### 4.2 Docs
- `README.md`: add a **Docker** deployment section (compose up, GHCR pull, volume
  layout) and the Windows dev notes; add a **deprecation note** on the
  tarball/systemd install pointing to Docker; reference the existing
  `uninstall.sh` for removing an old install.
- `CLAUDE.md`: update the **Server** section with the Docker workflow and the
  `PHOTOFRAME_DATA_DIR` env var.
- `DECISIONS.md`: short entry recording the move to containers, the armv7 +
  matrix CI decision, and the tarball-path deprecation.

---

## Section 5 — Tarball → Docker migration

`server/uninstall.sh` **already exists** (stops/removes the systemd service,
optionally deletes data) — no new uninstall script needed; just document it.

Add **`server/migrate-to-docker.sh`**:
- Usage: `./migrate-to-docker.sh /path/to/old/install [--force]`.
- Stops the `photoframe-server` systemd service first (clean SQLite state), if
  present.
- Copies `photoframe.db`, `images/`, and `thumbs/` from the old install dir into
  this repo's `server/data/` (the compose volume mount), creating `data/` if
  needed.
- Refuses to overwrite a **non-empty** `data/` unless `--force`.
- Prints the next step: `docker compose up -d`.

Migration is a clean **file copy** — verified that the DB stores bare relative
`filename` values (not absolute paths) and re-scans `IMAGES_DIR` via
`sync_images` on startup, and the schema is identical on both sides. No path
rewriting required.

Recommended sequence:
`uninstall.sh` (keep data) → `migrate-to-docker.sh /path/to/old/install` →
`docker compose up -d`.

---

## Testing strategy

- **Unit/API:** existing `server/tests/` (`test_api.py`, `test_database.py`) run
  in CI via pytest on Python 3.14.
- **Container smoke:** every CI run boots all three arch images and asserts
  `/healthz` returns 200.
- **Lint:** `ruff check` gates the `test` job.
- **Manual (one-off):** run `docker compose up` on the Windows laptop and on the
  Pi; verify the web UI, an upload, and a frame fetch against the containerised
  server; verify `migrate-to-docker.sh` against a copy of a real install.
