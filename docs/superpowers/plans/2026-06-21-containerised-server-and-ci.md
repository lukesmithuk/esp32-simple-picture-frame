# Containerised Server + CI + Windows Dev — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Containerise the `server/` photo-frame app (Dockerfile + compose), add a multi-arch GitHub CI pipeline publishing to GHCR, make server dev work on Windows, and provide a tarball→Docker migration path.

**Architecture:** Small code changes make the FastAPI server container-friendly (`/healthz`, a `PHOTOFRAME_DATA_DIR` volume root). A `python:3.14-slim` image is built for `linux/amd64`, `linux/arm64`, and `linux/arm/v7` by a parallel CI matrix (each leg on its own runner, smoke-booted, pushed by digest, then stitched into one GHCR manifest). The existing tarball/systemd path is kept but deprecated.

**Tech Stack:** FastAPI, uvicorn, Pillow, SQLite (aiosqlite), Docker/buildx, GitHub Actions, ruff, pytest.

## Global Constraints

- Container base image: `python:3.14-slim` (exact).
- Build platforms: `linux/amd64`, `linux/arm64`, `linux/arm/v7` (exact set).
- GHCR image: `ghcr.io/lukesmithuk/esp32-simple-picture-frame` (exact).
- Server listens on port `8080` inside the container.
- LAN-only: no cloudflared / no internet-facing services.
- Container files live in `server/` (build context = `server/`).
- Native dev behaviour must not change when `PHOTOFRAME_DATA_DIR` is unset (defaults to `BASE_DIR`).
- All work on a `feature/*` branch (already on `feature/containerised-server-ci`); merge to `main` only via PR after the three pre-merge reviews (code review, doc review, claude-md-improver).
- Tests: `httpx.ASGITransport(app=app)` + `AsyncClient`; `asyncio_mode = auto` (no `@pytest.mark.asyncio` needed).

---

### Task 1: `/healthz` endpoint

**Files:**
- Modify: `server/main.py` (add route after the `verify_api_key` auth function, ~line 52)
- Test: `server/tests/test_api.py` (append test)

**Interfaces:**
- Produces: `GET /healthz` → HTTP 200, JSON body `{"status": "ok"}`, no auth required. Consumed by the Docker healthcheck (Task 5) and CI smoke test (Task 6).

- [ ] **Step 1: Write the failing test**

Append to `server/tests/test_api.py`:

```python
async def test_healthz_no_auth():
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        resp = await ac.get("/healthz")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd server && python -m pytest tests/test_api.py::test_healthz_no_auth -v`
Expected: FAIL — 404 (route not defined).

- [ ] **Step 3: Add the endpoint**

In `server/main.py`, immediately after the `verify_api_key` function (end of the `# ── Auth ──` block), add:

```python
# ── Health ───────────────────────────────────────────────────────────────

@app.get("/healthz")
async def healthz():
    return {"status": "ok"}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd server && python -m pytest tests/test_api.py::test_healthz_no_auth -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add server/main.py server/tests/test_api.py
git commit -m "feat(server): add unauthenticated /healthz endpoint"
```

---

### Task 2: Configurable data directory

**Files:**
- Modify: `server/config.py` (lines 4-19)
- Test: `server/tests/test_config.py` (create)

**Interfaces:**
- Produces: `config.DATA_DIR`, and `config.IMAGES_DIR`/`config.THUMBS_DIR`/`config.DB_PATH` all rooted under `DATA_DIR`. `DATA_DIR = Path(os.environ.get("PHOTOFRAME_DATA_DIR", BASE_DIR))`. Unset env → identical paths to today. Consumed by Dockerfile/compose (Tasks 4-5) via `PHOTOFRAME_DATA_DIR=/data`.

- [ ] **Step 1: Write the failing test**

Create `server/tests/test_config.py`:

```python
import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


def test_data_dir_env_redirects_paths(tmp_path, monkeypatch):
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", str(tmp_path))
    import config
    try:
        importlib.reload(config)
        assert config.DATA_DIR == tmp_path
        assert config.DB_PATH == tmp_path / "photoframe.db"
        assert config.IMAGES_DIR == tmp_path / "images"
        assert config.THUMBS_DIR == tmp_path / "thumbs"
    finally:
        monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
        importlib.reload(config)  # restore module-level paths for other tests


def test_data_dir_defaults_to_base_dir(monkeypatch):
    monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
    import config
    importlib.reload(config)
    assert config.DATA_DIR == config.BASE_DIR
    assert config.DB_PATH == config.BASE_DIR / "photoframe.db"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd server && python -m pytest tests/test_config.py -v`
Expected: FAIL — `config.DATA_DIR` does not exist (AttributeError).

- [ ] **Step 3: Update `config.py`**

Replace the top of `server/config.py` (the path block) so it reads:

```python
import os
from pathlib import Path

BASE_DIR = Path(__file__).parent
DATA_DIR = Path(os.environ.get("PHOTOFRAME_DATA_DIR", BASE_DIR))
IMAGES_DIR = DATA_DIR / "images"
THUMBS_DIR = DATA_DIR / "thumbs"
DB_PATH = DATA_DIR / "photoframe.db"

API_KEY = os.environ.get("PHOTOFRAME_API_KEY", "changeme")
HOST = os.environ.get("PHOTOFRAME_HOST", "0.0.0.0")
PORT = int(os.environ.get("PHOTOFRAME_PORT", "8080"))

MAX_IMAGE_SIZE = 4 * 1024 * 1024  # 4 MB — matches frame's limit
DISPLAY_WIDTH = 800
DISPLAY_HEIGHT = 480

# Create directories on import
DATA_DIR.mkdir(parents=True, exist_ok=True)
IMAGES_DIR.mkdir(exist_ok=True)
THUMBS_DIR.mkdir(exist_ok=True)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd server && python -m pytest tests/test_config.py tests/test_api.py tests/test_database.py -v`
Expected: PASS (config tests pass; existing API/DB tests still pass — confirms no regression from the path refactor).

- [ ] **Step 5: Commit**

```bash
git add server/config.py server/tests/test_config.py
git commit -m "feat(server): root data paths at PHOTOFRAME_DATA_DIR for container volumes"
```

---

### Task 3: ruff lint config + fixes

**Files:**
- Create: `server/ruff.toml`
- Modify: `server/requirements.txt` (add `ruff`)
- Modify: any server `.py` files ruff flags

**Interfaces:**
- Produces: `ruff check` passes clean in `server/`. Consumed by CI `test` job (Task 6). Adds `ruff` to the dev-deps that the runtime image strips (Task 4) and the release tarball strips (Task 8).

- [ ] **Step 1: Create `server/ruff.toml`**

```toml
# Minimal lint config — pyflakes (F) + pycodestyle errors (E), not a style overhaul.
target-version = "py314"
line-length = 100

[lint]
select = ["E", "F", "I"]
ignore = ["E501"]  # long lines are not worth churning existing code over
```

- [ ] **Step 2: Add ruff to requirements**

Append `ruff` to `server/requirements.txt` (below the existing test deps):

```
ruff>=0.6.0
```

- [ ] **Step 3: Run ruff to see findings**

Run: `cd server && python -m pip install ruff && python -m ruff check .`
Expected: lists any unused imports / import-order issues (e.g. the function-local `from urllib.parse import quote` in `main.py`, unused imports in tests).

- [ ] **Step 4: Fix every finding**

Apply `python -m ruff check --fix .` for auto-fixable issues, then manually resolve anything remaining (move stray imports to module top, delete unused names). Do not suppress with `# noqa` unless a fix is genuinely unsafe.

- [ ] **Step 5: Verify clean + tests still pass**

Run: `cd server && python -m ruff check . && python -m pytest -q`
Expected: ruff prints "All checks passed!"; pytest all green.

- [ ] **Step 6: Commit**

```bash
git add server/ruff.toml server/requirements.txt server/main.py server/tests/
git commit -m "chore(server): add ruff lint config and fix findings"
```

---

### Task 4: Dockerfile + .dockerignore + .env.example

**Files:**
- Create: `server/Dockerfile`
- Create: `server/.dockerignore`
- Create: `server/.env.example`

**Interfaces:**
- Produces: an image that boots with `uvicorn main:app` on `0.0.0.0:8080` and serves `/healthz`. Consumed by compose (Task 5) and CI (Task 6). Reads `PHOTOFRAME_DATA_DIR` (Task 2) and `PHOTOFRAME_API_KEY`.

- [ ] **Step 1: Create `server/Dockerfile`**

```dockerfile
FROM python:3.14-slim
WORKDIR /app
ENV PYTHONUNBUFFERED=1

# Pillow needs JPEG/WebP/zlib at runtime. On armv7 there may be no prebuilt
# cp314 wheel, so include build deps + piwheels and compile as a fallback;
# purge the build-only deps in the same layer to keep the image small.
COPY requirements.txt ./
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libjpeg62-turbo libwebp7 zlib1g \
        gcc libjpeg-dev libwebp-dev zlib1g-dev \
    && grep -v -E '^(pytest|httpx|pytest-asyncio|ruff)' requirements.txt > requirements-runtime.txt \
    && pip install --no-cache-dir \
        --extra-index-url https://www.piwheels.org/simple \
        -r requirements-runtime.txt \
    && apt-get purge -y gcc libjpeg-dev libwebp-dev zlib1g-dev \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* requirements-runtime.txt

COPY main.py config.py database.py ./
COPY templates/ ./templates/
COPY static/ ./static/

ENV PHOTOFRAME_DATA_DIR=/data
RUN mkdir -p /data
EXPOSE 8080
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8080"]
```

- [ ] **Step 2: Create `server/.dockerignore`**

```
venv/
data/
__pycache__/
*.pyc
*.db
images/
thumbs/
tests/
.pytest_cache/
.env
```

- [ ] **Step 3: Create `server/.env.example`**

```
# Copy to server/.env (gitignored). Compose loads these into the container.
PHOTOFRAME_API_KEY=
PHOTOFRAME_PORT=8080
```

- [ ] **Step 4: Build and smoke-test locally**

Run (needs Docker Desktop; builds the native arch only):
```bash
cd server
docker build -t photoframe-smoke:latest .
docker run -d --rm --name pf-smoke -p 8080:8080 photoframe-smoke:latest
sleep 5
curl -fsS http://localhost:8080/healthz
docker rm -f pf-smoke
```
Expected: build succeeds; curl prints `{"status":"ok"}`.

> If Docker is unavailable in the execution environment, skip Step 4 and note it — CI (Task 6) will build and smoke all three arches.

- [ ] **Step 5: Commit**

```bash
git add server/Dockerfile server/.dockerignore server/.env.example
git commit -m "feat(server): add Dockerfile, .dockerignore, .env.example"
```

---

### Task 5: compose.yaml

**Files:**
- Create: `server/compose.yaml`

**Interfaces:**
- Consumes: the image from Task 4 and `PHOTOFRAME_DATA_DIR` from Task 2.
- Produces: a one-service LAN-only stack with a persistent `./data` volume and a `/healthz` healthcheck.

- [ ] **Step 1: Create `server/compose.yaml`**

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

- [ ] **Step 2: Validate compose config**

Run: `cd server && docker compose config`
Expected: prints the resolved config with no errors. (If Docker is unavailable, validate YAML with `python -c "import yaml,sys; yaml.safe_load(open('server/compose.yaml'))"` and note Docker was skipped.)

- [ ] **Step 3: End-to-end run (if Docker available)**

```bash
cd server
docker compose up -d --build
sleep 8
curl -fsS http://localhost:8080/healthz
docker compose down
```
Expected: `{"status":"ok"}`; a `server/data/` dir is created and persists.

- [ ] **Step 4: Commit**

```bash
git add server/compose.yaml
git commit -m "feat(server): add LAN-only compose.yaml with healthcheck and data volume"
```

---

### Task 6: CI pipeline

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `server/` (build context), `server/Dockerfile`, `/healthz` (smoke), `ruff.toml` + pytest (test job).
- Produces: on push to `main`, a multi-arch manifest at `ghcr.io/lukesmithuk/esp32-simple-picture-frame:latest` and `:${sha}`. On PR, build + smoke all three arches without pushing.

- [ ] **Step 1: Create `.github/workflows/ci.yml`**

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

env:
  IMAGE: ghcr.io/lukesmithuk/esp32-simple-picture-frame

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v5
      - uses: actions/setup-python@v6
        with:
          python-version: "3.14"
      - run: python -m pip install -r server/requirements.txt
      - name: Lint
        working-directory: server
        run: python -m ruff check .
      - name: Test
        working-directory: server
        run: python -m pytest -q

  # Build each platform on its own runner in parallel, boot-and-smoke the freshly
  # built image, then (push only) push it by digest for the merge job to stitch
  # into one multi-arch manifest. armv7 runs on the arm64 runner (aarch32,
  # near-native) so it builds fast and can be booted for the smoke test.
  build:
    runs-on: ${{ matrix.runner }}
    needs: test
    permissions:
      contents: read
      packages: write
    strategy:
      fail-fast: false
      matrix:
        include:
          - platform: linux/amd64
            runner: ubuntu-latest
          - platform: linux/arm64
            runner: ubuntu-24.04-arm
          - platform: linux/arm/v7
            runner: ubuntu-24.04-arm
    steps:
      - name: Prepare platform pair
        run: echo "PLATFORM_PAIR=${platform//\//-}" >> "$GITHUB_ENV"
        env:
          platform: ${{ matrix.platform }}
      - uses: actions/checkout@v5
      - uses: docker/setup-qemu-action@v3
      - uses: docker/setup-buildx-action@v4
      - uses: docker/login-action@v4
        if: github.event_name == 'push'
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - name: Build and load
        uses: docker/build-push-action@v7
        with:
          context: ./server
          platforms: ${{ matrix.platform }}
          load: true
          tags: photoframe-smoke:latest
          cache-from: type=gha,scope=${{ env.PLATFORM_PAIR }}
          cache-to: type=gha,mode=max,scope=${{ env.PLATFORM_PAIR }}
      - name: Smoke test
        run: |
          docker run -d --rm --name pf-smoke -p 8080:8080 photoframe-smoke:latest
          for i in $(seq 1 30); do
            if curl -fsS http://localhost:8080/healthz; then echo " OK"; exit 0; fi
            sleep 2
          done
          echo "healthz never came up"; docker logs pf-smoke; exit 1
      - name: Stop smoke container
        if: always()
        run: docker rm -f pf-smoke || true
      - name: Push by digest
        id: push
        if: github.event_name == 'push'
        uses: docker/build-push-action@v7
        with:
          context: ./server
          platforms: ${{ matrix.platform }}
          outputs: type=image,name=${{ env.IMAGE }},push-by-digest=true,name-canonical=true,push=true
          cache-from: type=gha,scope=${{ env.PLATFORM_PAIR }}
      - name: Export digest
        if: github.event_name == 'push'
        run: |
          mkdir -p "${{ runner.temp }}/digests"
          touch "${{ runner.temp }}/digests/${digest#sha256:}"
        env:
          digest: ${{ steps.push.outputs.digest }}
      - name: Upload digest
        if: github.event_name == 'push'
        uses: actions/upload-artifact@v7
        with:
          name: digests-${{ env.PLATFORM_PAIR }}
          path: ${{ runner.temp }}/digests/*
          if-no-files-found: error
          retention-days: 1

  merge:
    runs-on: ubuntu-latest
    needs: build
    if: github.event_name == 'push'
    permissions:
      contents: read
      packages: write
    steps:
      - name: Download digests
        uses: actions/download-artifact@v8
        with:
          path: ${{ runner.temp }}/digests
          pattern: digests-*
          merge-multiple: true
      - uses: docker/setup-buildx-action@v4
      - uses: docker/login-action@v4
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - name: Create manifest list and push
        working-directory: ${{ runner.temp }}/digests
        run: |
          docker buildx imagetools create \
            -t ${{ env.IMAGE }}:latest \
            -t ${{ env.IMAGE }}:${{ github.sha }} \
            $(printf '${{ env.IMAGE }}@sha256:%s ' *)
      - name: Inspect
        run: docker buildx imagetools inspect ${{ env.IMAGE }}:latest
```

- [ ] **Step 2: Validate workflow YAML**

Run: `python -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
Expected: no output (valid YAML).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: multi-arch (amd64/arm64/armv7) build, smoke, and GHCR publish"
```

> **Verification note:** the workflow runs for real only once pushed in the PR. Confirm on the PR that `test`, all three `build` legs, and `merge` (on the eventual merge to main) succeed, and that the GHCR package shows all three platforms via `docker buildx imagetools inspect`.

---

### Task 7: tarball → Docker migration script

**Files:**
- Create: `server/migrate-to-docker.sh`

**Interfaces:**
- Consumes: an old tarball install dir (positional arg 1) containing `photoframe.db`, `images/`, `thumbs/`.
- Produces: those copied into the compose data dir (default `server/data`, override with `PHOTOFRAME_DEST` for testability). Stops the systemd service first if present.

- [ ] **Step 1: Create `server/migrate-to-docker.sh`**

```bash
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

# Stop the old service so SQLite is not mid-write (no-op if not installed).
if command -v systemctl >/dev/null 2>&1 \
   && systemctl list-unit-files 2>/dev/null | grep -q "${SERVICE_NAME}.service"; then
    echo "Stopping ${SERVICE_NAME} service..."
    sudo systemctl stop "${SERVICE_NAME}" || true
fi

# Refuse to clobber a non-empty destination unless --force.
if [ -d "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ] && [ "$FORCE" != "--force" ]; then
    echo "Error: $DEST is not empty. Re-run with --force to overwrite."
    exit 1
fi

echo "Migrating data from $SRC -> $DEST"
mkdir -p "$DEST"
cp "$SRC/photoframe.db" "$DEST/photoframe.db"
[ -d "$SRC/images" ] && cp -r "$SRC/images" "$DEST/images"
[ -d "$SRC/thumbs" ] && cp -r "$SRC/thumbs" "$DEST/thumbs"

echo "Done. Next:"
echo "  cd $SCRIPT_DIR && docker compose up -d"
```

- [ ] **Step 2: Make executable**

Run: `chmod +x server/migrate-to-docker.sh`

- [ ] **Step 3: Functional test against a fake install**

Run (uses the Bash tool / git bash):
```bash
TMP=$(mktemp -d)
mkdir -p "$TMP/src/images" "$TMP/src/thumbs" "$TMP/dest"
echo "db" > "$TMP/src/photoframe.db"
echo "img" > "$TMP/src/images/a.jpg"
PHOTOFRAME_DEST="$TMP/dest" bash server/migrate-to-docker.sh "$TMP/src"
test -f "$TMP/dest/photoframe.db" && test -f "$TMP/dest/images/a.jpg" && echo "MIGRATE OK"
# non-empty dest without --force must fail:
PHOTOFRAME_DEST="$TMP/dest" bash server/migrate-to-docker.sh "$TMP/src" && echo "SHOULD HAVE FAILED" || echo "GUARD OK"
rm -rf "$TMP"
```
Expected: prints `MIGRATE OK` then `GUARD OK`.

- [ ] **Step 4: Commit**

```bash
git add server/migrate-to-docker.sh
git commit -m "feat(server): add tarball-to-Docker data migration script"
```

---

### Task 8: Docs — Docker workflow, Windows dev, deprecation

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md` (Server section)
- Modify: `DECISIONS.md`
- Modify: `.github/workflows/release.yml` (deprecation comment)

**Interfaces:**
- Consumes: everything above. Produces: accurate user-facing + agent-facing docs.

- [ ] **Step 1: Add a Docker section to `README.md`**

Under the server documentation, add (adjust heading depth to match the file):

````markdown
### Docker deployment (recommended)

Pull the published image or build locally with Compose:

```bash
cd server
cp .env.example .env          # set PHOTOFRAME_API_KEY
docker compose up -d          # pulls ghcr.io/lukesmithuk/esp32-simple-picture-frame
```

Photos, thumbnails, and the SQLite DB persist in `server/data/` (mounted to
`/data` in the container). The server listens on `http://<host>:8080`.

Images are published by CI for `linux/amd64`, `linux/arm64`, and `linux/arm/v7`
(Raspberry Pi Zero 2W, 32-bit OS).

**Migrating from a tarball install:**

```bash
cd server
./migrate-to-docker.sh /path/to/old/photoframe-server   # copies DB + images
docker compose up -d
```

### Tarball + systemd install (deprecated)

> **Deprecated** in favour of Docker (above); kept for non-Docker hosts and
> slated for future removal. To remove an existing systemd install:
> `./uninstall.sh` (keep your data, then migrate it as above).
````

- [ ] **Step 2: Add Windows dev notes to `README.md`**

```markdown
### Server development on Windows

Primary path is Docker Desktop (`cd server && docker compose up`). Without Docker:

```powershell
cd server
py -m venv venv
venv\Scripts\python -m pip install -r requirements.txt
venv\Scripts\python -m pytest                 # run tests
$env:PHOTOFRAME_API_KEY="changeme"; venv\Scripts\python main.py   # run locally
```

The `*.sh` scripts (`setup.sh`, `run.sh`, `uninstall.sh`) target Linux.
```

- [ ] **Step 3: Update `CLAUDE.md` Server section**

In `CLAUDE.md`, in the `## Server` section, add a Docker subsection noting:
- `cd server && docker compose up -d` (image `ghcr.io/lukesmithuk/esp32-simple-picture-frame`).
- `PHOTOFRAME_DATA_DIR` roots the DB/images/thumbs (container sets `/data`, volume `./data`).
- CI builds amd64/arm64/armv7 and publishes to GHCR on push to `main`.
- Tarball/systemd path is deprecated.

Concretely, add after the existing server install block:

```markdown
**Docker (recommended):**
```bash
cd server && cp .env.example .env && docker compose up -d
```
Image: `ghcr.io/lukesmithuk/esp32-simple-picture-frame` (amd64/arm64/armv7, built by CI).
Data (DB, images, thumbs) persists in `server/data/` via `PHOTOFRAME_DATA_DIR=/data`.
The tarball + systemd install is **deprecated** (kept for non-Docker hosts).
```

- [ ] **Step 4: Add a `DECISIONS.md` entry**

Append a dated entry recording: move to containerised deployment (Docker/compose + GHCR); CI builds `linux/amd64,linux/arm64,linux/arm/v7` via a parallel runner matrix (armv7 has no native runner, built on the arm64 runner via aarch32); LAN-only (no tunnel); tarball/systemd path deprecated but retained for now.

- [ ] **Step 5: Deprecation comment in `release.yml`**

At the top of `.github/workflows/release.yml`, under `name:`, add:

```yaml
# DEPRECATED: the tarball release is superseded by the GHCR container image
# published by ci.yml. Kept for non-Docker installs; slated for future removal.
```

- [ ] **Step 6: Commit**

```bash
git add README.md CLAUDE.md DECISIONS.md .github/workflows/release.yml
git commit -m "docs: Docker deployment, Windows dev, and tarball deprecation"
```

---

## Pre-merge (before opening/merging the PR)

Per the repo workflow rule (CLAUDE.md → Git & Contribution Workflow), before merging run all three and address findings:

- [ ] **Code review** — `/code-review` on the full branch diff.
- [ ] **Document review** — re-read README, CLAUDE.md, DECISIONS.md, and the spec for accuracy/consistency against the final code.
- [ ] **CLAUDE.md improver** — run the `claude-md-management:claude-md-improver` plugin.
- [ ] Open the PR (`feature/containerised-server-ci` → `main`); confirm CI `test` + three `build` legs are green.

---

## Self-Review notes (author)

- **Spec coverage:** §1 → Tasks 1-3; §2 → Tasks 4-5; §3 → Task 6; §4 → Task 8; §5 → Task 7 (+ `uninstall.sh` documented in Task 8). All sections covered.
- **Type/name consistency:** `PHOTOFRAME_DATA_DIR`, `/healthz`, image name, port `8080`, and the `pytest|httpx|pytest-asyncio|ruff` strip pattern are identical across config, Dockerfile, compose, CI, and release.yml.
- **No placeholders:** every file has complete contents; the only discovery-based step is ruff fix output (Task 3), which is inherent to linting and bounded by the `E,F,I` rule set.
