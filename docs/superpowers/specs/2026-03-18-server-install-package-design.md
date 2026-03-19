# Server Install Package — Design Spec

## Overview

Create a distributable install package for the photo frame server so it can be installed on a Raspberry Pi without git or GitHub access. Includes a single-command installer and automated GitHub Release publishing.

## Goals

- One-command install on a fresh Pi: download, extract, run `./setup.sh`
- No git, GitHub login, or manual configuration needed
- Automated release builds when a version tag is pushed

## Non-Goals

- PyPI publication
- Docker container
- Debian package

---

## Archive

**Filename:** `photoframe-server-{tag}.tar.gz` (e.g. `photoframe-server-v1.0.0.tar.gz`)

**Contents:**

```
photoframe-server/
  setup.sh              # Single-command installer
  config.py             # Server configuration
  database.py           # SQLite schema + queries
  main.py               # FastAPI app
  requirements.txt      # Runtime dependencies only (no test deps)
  run.sh                # Start script (used by systemd)
  templates/
    index.html
    logs.html
  static/
    style.css
```

**Excluded:** venv, `__pycache__`, tests, pytest.ini, images, thumbs, photoframe.db, server.env, install.sh, install-service.sh (replaced by setup.sh).

**Archive creation:** The GitHub Action copies server files to a staging directory named `photoframe-server/`, adds `setup.sh`, strips test dependencies from `requirements.txt`, then tars.

---

## Requirements Split

The distributed `requirements.txt` excludes test-only dependencies:

**Distributed (runtime only):**
```
fastapi>=0.104.0
uvicorn[standard]>=0.24.0
aiosqlite>=0.19.0
python-multipart>=0.0.6
jinja2>=3.1.2
pillow>=10.0.0
```

**Development (kept in repo as `requirements-dev.txt` or left in original `requirements.txt`):**
```
pytest>=7.0.0
httpx>=0.25.0
pytest-asyncio>=1.0.0
```

The GitHub Action strips the test deps when building the archive.

---

## setup.sh

Single script that handles the full install:

1. Refuse to run as root (`if [ "$(id -u)" = 0 ]` → print "don't run as root, use your normal user" and exit)
2. Check Python >= 3.10 + python3-venv are installed. If not, print: `sudo apt install python3 python3-venv` and exit.
3. Create virtual environment (`venv/`)
4. Install dependencies from `requirements.txt`
5. If `server.env` exists: read current API key and port as defaults (idempotent re-run)
6. Prompt for API key (default: existing value, or random 16-char hex for first install)
7. Prompt for port (default: existing value, or 8080 for first install)
8. Write `server.env` with API key, port, host=0.0.0.0
9. Install systemd service (`photoframe-server.service`) via `sudo`
10. Enable and start the service
11. Print: server URL (first address from `hostname -I`), API key, management commands

**Idempotency:** On re-run, existing `server.env` values are used as prompt defaults so the user can press Enter to keep them. The API key is never silently replaced.

**Root detection:** If run as root, the script exits with a message. If `sudo` is needed (for systemd), the script calls `sudo` internally for just those commands.

---

## GitHub Action

**File:** `.github/workflows/release.yml`

**Trigger:** Push of a tag matching `v*` (e.g. `v1.0.0`)

**Steps:**

1. Checkout repository
2. Create staging directory `photoframe-server/`
3. Copy server files (excluding venv, pycache, tests, pytest.ini, images, thumbs, DB, env, install.sh, install-service.sh)
4. Copy `setup.sh` into staging directory
5. Strip test dependencies from `requirements.txt` in staging
6. Create `photoframe-server-{tag}.tar.gz` and `photoframe-server.tar.gz`
7. Create GitHub Release with tag name as title
8. Attach both archives

**Note:** The "latest" URL (`releases/latest/download/photoframe-server.tar.gz`) works as long as tags are pushed in chronological order and no release is marked as pre-release.

**Release URL pattern:**
- Latest: `https://github.com/lukesmithuk/esp32-simple-picture-frame/releases/latest/download/photoframe-server.tar.gz`
- Specific: `https://github.com/lukesmithuk/esp32-simple-picture-frame/releases/download/v1.0.0/photoframe-server-v1.0.0.tar.gz`

---

## User Installation Flow

```bash
curl -L https://github.com/lukesmithuk/esp32-simple-picture-frame/releases/latest/download/photoframe-server.tar.gz | tar xz
cd photoframe-server
./setup.sh
```

**Upgrade path:** Download new archive, extract over existing directory (preserves `server.env`, `photoframe.db`, `images/`, `thumbs/`), re-run `./setup.sh` (updates venv, restarts service).

---

## Additional Fixes Included

- **Upload endpoint auth:** Add `Depends(verify_api_key)` to `POST /api/upload` (currently unauthenticated)
- **Python version check:** setup.sh verifies Python >= 3.10 (code uses PEP 604 `bool | None` syntax)

---

## Files to Create

- `server/setup.sh` — single-command installer
- `.github/workflows/release.yml` — GitHub Action for automated releases

## Files to Modify

- `server/main.py` — add auth to upload endpoint
- `server/requirements.txt` — split into runtime and dev (or strip during archive build)

---

## Release Workflow

```bash
git tag v1.0.0
git push origin v1.0.0
```

GitHub Action runs, creates release with downloadable archive.
