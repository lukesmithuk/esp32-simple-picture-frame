# Low-Battery Email Alerts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The photo server emails the owner one combined message when any frame's battery drops below a configurable threshold (default 20%). After that it sends a reminder every 24h while any frame stays low and isn't charging.

**Architecture:** `/api/status` stores the frame's report, then schedules `notifier.check_battery_alerts(db)` as a FastAPI background task. The check evaluates **all** frames using a pure `evaluate()` function and sends one plain-text email via stdlib `smtplib` (in a worker thread). It then records a per-frame `low_battery_alerted_at` timestamp, and an `asyncio.Lock` prevents duplicate sends. SMTP credentials come from `.env`. The recipient and threshold are edited in a new dashboard card and stored in the `settings` table.

**Tech Stack:** Python 3.14, FastAPI, aiosqlite, Jinja2, stdlib `smtplib`/`email`/`ssl`, pytest + pytest-asyncio (`asyncio_mode = auto`), httpx `ASGITransport`, ruff.

**Spec:** `docs/superpowers/specs/2026-09-24-battery-alert-email-design.md`

## Global Constraints

- Branch: `feature/battery-alert-email` (already created; never commit to `main`).
- **No new dependencies.** `requirements.txt` is unchanged; email uses stdlib only.
- All commands run from `server/`. Tests: `venv/Scripts/python -m pytest -q` (Linux: `venv/bin/python`). Lint: `venv/Scripts/python -m ruff check .`. If ruff reports only import order (`I001`), run `ruff check --fix .` and review the diff.
- Timestamps: UTC ISO 8601 with `+00:00` suffix (`datetime.now(timezone.utc).isoformat()`).
- Logging: `logging.getLogger("uvicorn.error")`, as in `database.py`.
- Threshold: integer 1–99, default **20**. Re-arm margin **+5** points. Reminder interval **24h**. SMTP timeout **15s**.
- Recipient validation: comma-separated; each address trimmed, exactly one `@`, not starting or ending with `@`, no whitespace, `\r` or `\n`. An empty value disables alerts.
- Alerts are enabled only when `SMTP_HOST` is set, the From address (`SMTP_FROM`, or `SMTP_USER` if that is empty) is set, **and** a recipient is saved.
- Dashboard hint strings (exact):
  - `SMTP not configured. Set PHOTOFRAME_SMTP_HOST (and SMTP_USER or SMTP_FROM) in .env.`
  - `Alerts off: no recipient set.`
  - `Alerts on: emailing <recipient> when a frame drops below <T>%.`
- Email subject strings (exact): `Photo frame battery low: <label> (<pct>%)`, `Photo frame battery low: <n> frames`, `Photo frame test email`.
- Commit messages end with:
  ```
  Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw
  ```

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `server/tests/conftest.py` | Create | Points `PHOTOFRAME_DATA_DIR` at a temp dir for the whole test session; resets the notifier lock per test |
| `server/tests/test_config.py` | Modify | Reload-safe fixture; SMTP env parsing tests |
| `server/config.py` | Modify | `SMTP_*` settings from env |
| `server/.env.example` | Modify | Document `PHOTOFRAME_SMTP_*` |
| `server/database.py` | Modify | `low_battery_alerted_at` column + migration; alert state and alert settings methods |
| `server/tests/test_database.py` | Modify | Tests for the new DB methods and migration |
| `server/notifier.py` | Create | All alert logic: config snapshot, recipient parsing, `evaluate`, message building, `send_email`, `check_battery_alerts` |
| `server/tests/test_notifier.py` | Create | Unit tests (pure functions, `send_email`) + DB-backed `check_battery_alerts` tests |
| `server/main.py` | Modify | Background task in `/api/status`; `/settings/alerts` and `/settings/alerts/test` routes; dashboard context + `notice` |
| `server/templates/index.html` | Modify | Battery alerts card; notice toast |
| `server/static/style.css` | Modify | Card spacing + email input width |
| `server/tests/test_api.py` | Modify | End-to-end tests for status → email, settings routes, test-email route, dashboard |
| `DECISIONS.md`, `README.md`, `CLAUDE.md`, `TODO.md`, `PROGRESS.md` | Modify | Docs |

---

### Task 1: Isolate the test suite from real server data

**Why:** `tests/test_api.py`'s `clean_state` fixture deletes `config.DB_PATH`, `IMAGES_DIR` and `THUMBS_DIR` before every test. Without `PHOTOFRAME_DATA_DIR` set, these are the developer's real `server/photoframe.db`, `server/images/` and `server/thumbs/`, and running the suite wipes them. This plan runs the suite many times, so fix it first. `test_config.py` also leaves `config` reloaded against the wrong directory: its `finally: reload()` runs before `monkeypatch` has restored the environment.

**Files:**
- Create: `server/tests/conftest.py`
- Modify: `server/tests/test_config.py` (whole file)

**Interfaces:**
- Produces: every test runs with `config.DATA_DIR` = a temp directory. The `fresh_config` fixture in `test_config.py` yields the `config` module and, on teardown, undoes env changes and reloads it (reused by Task 2).

- [ ] **Step 1: Write the failing test**

Replace `server/tests/test_config.py` entirely with:

```python
import importlib
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))


@pytest.fixture
def fresh_config(monkeypatch):
    """Yield the config module; afterwards undo env changes, then reload it.

    Undo must happen *before* the reload, otherwise config is left pointing
    at whatever the test set (e.g. the real server/ data dir).
    """
    import config
    yield config
    monkeypatch.undo()
    importlib.reload(config)


def test_suite_runs_against_isolated_data_dir():
    # conftest.py points PHOTOFRAME_DATA_DIR at a temp dir so test_api's
    # clean_state fixture can never delete the developer's real DB/images.
    import config
    assert config.DATA_DIR != config.BASE_DIR


def test_data_dir_env_redirects_paths(fresh_config, tmp_path, monkeypatch):
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", str(tmp_path))
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == tmp_path
    assert config.DB_PATH == tmp_path / "photoframe.db"
    assert config.IMAGES_DIR == tmp_path / "images"
    assert config.THUMBS_DIR == tmp_path / "thumbs"


def test_data_dir_defaults_to_base_dir(fresh_config, monkeypatch):
    monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == config.BASE_DIR
    assert config.DB_PATH == config.BASE_DIR / "photoframe.db"


def test_data_dir_empty_string_falls_back_to_base_dir(fresh_config, monkeypatch):
    # An empty value (e.g. `PHOTOFRAME_DATA_DIR=` in .env) must not resolve to CWD.
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", "")
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == config.BASE_DIR
```

- [ ] **Step 2: Run test to verify it fails**

Run: `venv/Scripts/python -m pytest tests/test_config.py -q`
Expected: `test_suite_runs_against_isolated_data_dir` FAILS (`DATA_DIR == BASE_DIR`); the other three pass.

- [ ] **Step 3: Create `server/tests/conftest.py`**

```python
"""Shared test setup.

PHOTOFRAME_DATA_DIR is pointed at a throwaway directory *before* any test
module imports config/main. test_api.py's clean_state fixture deletes the DB,
images and thumbs between tests; without this it would delete the
developer's real server/ data.
"""
import os
import shutil
import sys
import tempfile
from pathlib import Path

_TEST_DATA_DIR = tempfile.mkdtemp(prefix="photoframe-test-")
os.environ["PHOTOFRAME_DATA_DIR"] = _TEST_DATA_DIR

sys.path.insert(0, str(Path(__file__).parent.parent))


def pytest_sessionfinish(session, exitstatus):
    shutil.rmtree(_TEST_DATA_DIR, ignore_errors=True)
```

- [ ] **Step 4: Run the full suite to verify**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass (27 = 26 existing + 1 new).

Then confirm the real data dir was not touched. Run `ls -la photoframe.db images thumbs` before and after a further `pytest -q` run: the modification times must be unchanged.

- [ ] **Step 5: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add tests/conftest.py tests/test_config.py
git commit -m "test: isolate suite in a temp data dir so tests never wipe server/ data" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 2: SMTP configuration from environment

**Files:**
- Modify: `server/config.py` (append after `PORT = ...`, and add `import logging`)
- Modify: `server/.env.example`
- Test: `server/tests/test_config.py` (append)

**Interfaces:**
- Consumes: the `fresh_config` fixture from Task 1.
- Produces: module attributes `config.SMTP_HOST: str`, `config.SMTP_PORT: int`, `config.SMTP_TLS: str` (`"starttls"` or `"ssl"`), `config.SMTP_USER: str`, `config.SMTP_PASSWORD: str`, `config.SMTP_FROM: str` (falls back to `SMTP_USER`).

- [ ] **Step 1: Write the failing tests**

Append to `server/tests/test_config.py`:

```python
SMTP_VARS = (
    "PHOTOFRAME_SMTP_HOST", "PHOTOFRAME_SMTP_PORT", "PHOTOFRAME_SMTP_TLS",
    "PHOTOFRAME_SMTP_USER", "PHOTOFRAME_SMTP_PASSWORD", "PHOTOFRAME_SMTP_FROM",
)


def _clear_smtp_env(monkeypatch):
    for var in SMTP_VARS:
        monkeypatch.delenv(var, raising=False)


def test_smtp_defaults(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    config = importlib.reload(fresh_config)
    assert config.SMTP_HOST == ""
    assert config.SMTP_PORT == 587
    assert config.SMTP_TLS == "starttls"
    assert config.SMTP_USER == ""
    assert config.SMTP_PASSWORD == ""
    assert config.SMTP_FROM == ""


def test_smtp_from_env(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_HOST", "smtp.example.com")
    monkeypatch.setenv("PHOTOFRAME_SMTP_PORT", "465")
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "SSL")
    monkeypatch.setenv("PHOTOFRAME_SMTP_USER", "user@example.com")
    monkeypatch.setenv("PHOTOFRAME_SMTP_PASSWORD", "secret")
    monkeypatch.setenv("PHOTOFRAME_SMTP_FROM", "frames@example.com")
    config = importlib.reload(fresh_config)
    assert config.SMTP_HOST == "smtp.example.com"
    assert config.SMTP_PORT == 465
    assert config.SMTP_TLS == "ssl"
    assert config.SMTP_USER == "user@example.com"
    assert config.SMTP_PASSWORD == "secret"
    assert config.SMTP_FROM == "frames@example.com"


def test_smtp_from_falls_back_to_user(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_USER", "user@example.com")
    config = importlib.reload(fresh_config)
    assert config.SMTP_FROM == "user@example.com"


def test_smtp_blank_values_use_defaults(fresh_config, monkeypatch):
    # `PHOTOFRAME_SMTP_PORT=` (blank line in .env) must not crash int().
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_PORT", "")
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "")
    config = importlib.reload(fresh_config)
    assert config.SMTP_PORT == 587
    assert config.SMTP_TLS == "starttls"


def test_smtp_invalid_tls_falls_back_to_starttls(fresh_config, monkeypatch, caplog):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "bogus")
    config = importlib.reload(fresh_config)
    assert config.SMTP_TLS == "starttls"
    assert "PHOTOFRAME_SMTP_TLS" in caplog.text
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_config.py -q`
Expected: the five new tests FAIL with `AttributeError: module 'config' has no attribute 'SMTP_HOST'`.

- [ ] **Step 3: Implement in `server/config.py`**

Add `import logging` at the top (before `import os`). Insert this block after the `PORT = ...` line:

```python

# Email alerts (SMTP). An empty host disables email entirely.
SMTP_HOST = os.environ.get("PHOTOFRAME_SMTP_HOST", "")
SMTP_PORT = int(os.environ.get("PHOTOFRAME_SMTP_PORT") or "587")
SMTP_TLS = (os.environ.get("PHOTOFRAME_SMTP_TLS") or "starttls").strip().lower()
if SMTP_TLS not in ("starttls", "ssl"):
    logging.getLogger("uvicorn.error").warning(
        "PHOTOFRAME_SMTP_TLS=%r not recognised (use starttls or ssl); using starttls",
        SMTP_TLS,
    )
    SMTP_TLS = "starttls"
SMTP_USER = os.environ.get("PHOTOFRAME_SMTP_USER", "")
SMTP_PASSWORD = os.environ.get("PHOTOFRAME_SMTP_PASSWORD", "")
SMTP_FROM = os.environ.get("PHOTOFRAME_SMTP_FROM") or SMTP_USER
```

- [ ] **Step 4: Document in `server/.env.example`**

Replace the file with:

```bash
# Copy to server/.env (gitignored). Compose loads these into the container.
PHOTOFRAME_API_KEY=
PHOTOFRAME_PORT=8080

# ── Low-battery email alerts (optional) ──────────────────────────────────
# Leave PHOTOFRAME_SMTP_HOST empty to disable email entirely. The recipient
# address and battery threshold are set on the web dashboard.
# Gmail: host smtp.gmail.com, port 587, starttls, user = your address,
# password = an app password (https://myaccount.google.com/apppasswords).
PHOTOFRAME_SMTP_HOST=
PHOTOFRAME_SMTP_PORT=587
# starttls (usually port 587) or ssl (implicit TLS, usually port 465)
PHOTOFRAME_SMTP_TLS=starttls
# Leave user/password empty for an unauthenticated relay
PHOTOFRAME_SMTP_USER=
PHOTOFRAME_SMTP_PASSWORD=
# "From" address; defaults to PHOTOFRAME_SMTP_USER (required if user is empty)
PHOTOFRAME_SMTP_FROM=
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

- [ ] **Step 6: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add config.py .env.example tests/test_config.py
git commit -m "feat(server): read SMTP settings for email alerts from env" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 3: Database — alert state column and alert settings

**Files:**
- Modify: `server/database.py` (`frames` CREATE TABLE ~line 25; `init()` ~line 62; new migration method after `_migrate_frame_wake_columns`; new methods in the Frames and Settings sections)
- Test: `server/tests/test_database.py` (append)

**Interfaces:**
- Produces:
  - `frames.low_battery_alerted_at TEXT` (nullable), included in `list_frames()` / `get_frame()` dicts.
  - `async Database.clear_low_battery_alerts(frame_ids: list[int]) -> None`
  - `async Database.set_low_battery_alerted(frame_ids: list[int], timestamp: str) -> None`
  - `async Database.get_alert_settings() -> dict` returning `{"email": str, "threshold": int}` (defaults `""`, `20`)
  - `async Database.set_alert_settings(email: str, threshold: int) -> None`

- [ ] **Step 1: Write the failing tests**

Append to `server/tests/test_database.py` (add `import aiosqlite` to the imports at the top):

```python
# -- Low-battery alerts --


@pytest.mark.asyncio
async def test_frames_have_low_battery_alerted_at_column(db):
    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "k")
    frame = await db.get_frame(frame_id)
    assert "low_battery_alerted_at" in frame
    assert frame["low_battery_alerted_at"] is None


@pytest.mark.asyncio
async def test_migration_adds_low_battery_alerted_at(tmp_path):
    # A database created before this feature: frames table without the column.
    path = tmp_path / "old.db"
    conn = await aiosqlite.connect(path)
    await conn.execute(
        "CREATE TABLE frames (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "mac_address TEXT NOT NULL UNIQUE, api_key TEXT NOT NULL, name TEXT, "
        "last_seen TEXT, battery_percent INTEGER, battery_mv INTEGER, "
        "charging INTEGER, usb_connected INTEGER, battery_connected INTEGER, "
        "sd_free_kb INTEGER, firmware_version TEXT, logs TEXT DEFAULT '')"
    )
    await conn.execute(
        "INSERT INTO frames (mac_address, api_key) VALUES ('AA:BB:CC:DD:EE:FF', 'k')"
    )
    await conn.commit()
    await conn.close()

    d = Database(path)
    await d.init()
    try:
        frames = await d.list_frames()
        assert frames[0]["low_battery_alerted_at"] is None
    finally:
        await d.close()


@pytest.mark.asyncio
async def test_set_and_clear_low_battery_alerts(db):
    a = await db.get_or_create_frame("AA:00:00:00:00:01", "k")
    b = await db.get_or_create_frame("AA:00:00:00:00:02", "k")
    c = await db.get_or_create_frame("AA:00:00:00:00:03", "k")
    ts = "2026-09-24T12:00:00+00:00"

    await db.set_low_battery_alerted([a, b], ts)
    assert (await db.get_frame(a))["low_battery_alerted_at"] == ts
    assert (await db.get_frame(b))["low_battery_alerted_at"] == ts
    assert (await db.get_frame(c))["low_battery_alerted_at"] is None

    await db.clear_low_battery_alerts([a])
    assert (await db.get_frame(a))["low_battery_alerted_at"] is None
    assert (await db.get_frame(b))["low_battery_alerted_at"] == ts


@pytest.mark.asyncio
async def test_alert_settings_defaults(db):
    assert await db.get_alert_settings() == {"email": "", "threshold": 20}


@pytest.mark.asyncio
async def test_alert_settings_round_trip(db):
    await db.set_alert_settings("me@example.com, you@example.org", 15)
    assert await db.get_alert_settings() == {
        "email": "me@example.com, you@example.org",
        "threshold": 15,
    }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_database.py -q`
Expected: the five new tests FAIL (`KeyError`/`AssertionError` on the missing column; `AttributeError` for the missing methods).

- [ ] **Step 3: Implement in `server/database.py`**

(a) In the `CREATE TABLE IF NOT EXISTS frames` statement, add a column after `logs TEXT DEFAULT ''`:

```sql
                logs TEXT DEFAULT '',
                low_battery_alerted_at TEXT
```

(b) In `init()`, after `await self._migrate_frame_wake_columns()`, add:

```python

        # Migration: add low-battery alert state column if missing.
        await self._migrate_low_battery_alert_column()
```

(c) After the `_migrate_frame_wake_columns` method, add:

```python
    async def _migrate_low_battery_alert_column(self):
        """Add low_battery_alerted_at to frames if missing."""
        cursor = await self.db.execute("PRAGMA table_info(frames)")
        columns = {row["name"] for row in await cursor.fetchall()}
        if "low_battery_alerted_at" not in columns:
            await self.db.execute("ALTER TABLE frames ADD COLUMN low_battery_alerted_at TEXT")
            await self.db.commit()
            logger.info("Migrated: added low_battery_alerted_at column to frames table")
```

(d) At the end of the `# -- Frames --` section (after `update_frame_status`), add:

```python
    async def clear_low_battery_alerts(self, frame_ids: list[int]):
        """Re-arm low-battery alerts for these frames."""
        await self.db.executemany(
            "UPDATE frames SET low_battery_alerted_at = NULL WHERE id = ?",
            [(fid,) for fid in frame_ids],
        )
        await self.db.commit()

    async def set_low_battery_alerted(self, frame_ids: list[int], timestamp: str):
        """Record that a low-battery alert covering these frames was sent."""
        await self.db.executemany(
            "UPDATE frames SET low_battery_alerted_at = ? WHERE id = ?",
            [(timestamp, fid) for fid in frame_ids],
        )
        await self.db.commit()
```

(e) At the end of the `# -- Settings --` section (after `set_wake_interval`), add:

```python
    async def get_alert_settings(self) -> dict:
        """Returns {email, threshold}. Default: no recipient, 20%."""
        return {
            "email": await self.get_setting("alert_email", ""),
            "threshold": int(await self.get_setting("battery_alert_threshold", "20")),
        }

    async def set_alert_settings(self, email: str, threshold: int):
        await self.set_setting("alert_email", email)
        await self.set_setting("battery_alert_threshold", str(threshold))
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

- [ ] **Step 5: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add database.py tests/test_database.py
git commit -m "feat(server): store low-battery alert state and alert settings" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 4: Notifier — pure alert logic and message building

**Files:**
- Create: `server/notifier.py`
- Test: `server/tests/test_notifier.py` (create)

**Interfaces:**
- Consumes: `config.SMTP_*` (Task 2). Frame dicts shaped like `Database.list_frames()` rows (Task 3), with keys `id`, `name`, `mac_address`, `battery_percent`, `battery_mv`, `battery_connected`, `charging`, `usb_connected`, `last_seen`, `low_battery_alerted_at`. SQLite booleans arrive as `0`/`1`/`None`.
- Produces:
  - `SmtpConfig(host, port, tls, user, password, sender)` frozen dataclass with property `configured -> bool` (host and sender both non-empty)
  - `smtp_config() -> SmtpConfig`, reading `config.SMTP_*` **at call time** so tests can monkeypatch them
  - `parse_recipients(value: str) -> list[str] | None`, where `[]` means empty (alerts off) and `None` means invalid
  - `AlertDecision` dataclass: `rearm_ids: list[int]`, `low_frames: list[dict]` (sorted by `battery_percent` ascending), `new_ids: set[int]`, `send: bool`
  - `evaluate(frames: list[dict], threshold: int, now: datetime) -> AlertDecision`
  - `build_alert_message(decision: AlertDecision, threshold: int, cfg: SmtpConfig, recipients: list[str]) -> EmailMessage`
  - `build_test_message(threshold: int, cfg: SmtpConfig, recipients: list[str]) -> EmailMessage`
  - Constants `REARM_MARGIN = 5`, `REMINDER_INTERVAL = timedelta(hours=24)`

- [ ] **Step 1: Write the failing tests**

Create `server/tests/test_notifier.py`:

```python
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import notifier
from notifier import AlertDecision, SmtpConfig, evaluate, parse_recipients

NOW = datetime(2026, 9, 24, 12, 0, tzinfo=timezone.utc)
T = 20  # threshold used throughout

CFG = SmtpConfig(
    host="smtp.test", port=587, tls="starttls",
    user="user@test", password="pw", sender="frames@test",
)


def frame(id=1, pct=15, *, connected=True, charging=False, usb=False,
          alerted=None, name=None, mac="AA:00:00:00:00:01", mv=3600, last_seen=None):
    """A frame dict shaped like a Database.list_frames() row."""
    return {
        "id": id,
        "name": name,
        "mac_address": mac,
        "battery_percent": pct,
        "battery_mv": mv,
        "battery_connected": int(connected),
        "charging": int(charging),
        "usb_connected": int(usb),
        "last_seen": (last_seen or NOW).isoformat(),
        "low_battery_alerted_at": alerted.isoformat() if alerted else None,
    }


# -- SmtpConfig --

def test_smtp_config_configured_needs_host_and_sender():
    assert CFG.configured
    assert not SmtpConfig("", 587, "starttls", "u", "p", "s@test").configured
    assert not SmtpConfig("smtp.test", 587, "starttls", "", "", "").configured


def test_smtp_config_reads_config_at_call_time(monkeypatch):
    import config
    monkeypatch.setattr(config, "SMTP_HOST", "smtp.later")
    monkeypatch.setattr(config, "SMTP_FROM", "later@test")
    cfg = notifier.smtp_config()
    assert cfg.host == "smtp.later"
    assert cfg.sender == "later@test"


# -- parse_recipients --

@pytest.mark.parametrize("value,expected", [
    ("", []),
    ("   ", []),
    (" , ", []),
    (" me@example.com ", ["me@example.com"]),
    ("me@example.com, you@example.org", ["me@example.com", "you@example.org"]),
    ("me@example.com,,", ["me@example.com"]),
])
def test_parse_recipients_valid(value, expected):
    assert parse_recipients(value) == expected


@pytest.mark.parametrize("value", [
    "nope",
    "a@b@c.com",
    "@example.com",
    "me@",
    "me @example.com",
    "me@example.com\nBcc: evil@example.com",
    "me@example.com\r\nBcc: evil@example.com",
    "me@example.com, broken",
])
def test_parse_recipients_invalid(value):
    assert parse_recipients(value) is None


# -- evaluate: single-frame rules --

def test_below_threshold_new_frame_sends():
    d = evaluate([frame(pct=T - 1)], T, NOW)
    assert d.send
    assert [f["id"] for f in d.low_frames] == [1]
    assert d.new_ids == {1}
    assert d.rearm_ids == []


def test_at_threshold_is_not_low():
    d = evaluate([frame(pct=T)], T, NOW)
    assert not d.send
    assert d.low_frames == []


def test_within_rearm_margin_keeps_state():
    d = evaluate([frame(pct=T + 4, alerted=NOW - timedelta(hours=1))], T, NOW)
    assert d.rearm_ids == []
    assert d.low_frames == []
    assert not d.send


def test_at_rearm_margin_rearms():
    d = evaluate([frame(pct=T + 5, alerted=NOW - timedelta(hours=1))], T, NOW)
    assert d.rearm_ids == [1]


def test_rearm_only_lists_frames_that_were_alerted():
    # Clearing an already-NULL timestamp is a no-op, so it is not listed.
    d = evaluate([frame(pct=90, alerted=None)], T, NOW)
    assert d.rearm_ids == []


@pytest.mark.parametrize("kwargs", [
    {"charging": True},
    {"usb": True},
    {"connected": False},
])
def test_power_or_no_battery_rearms_and_suppresses(kwargs):
    d = evaluate([frame(pct=5, alerted=NOW - timedelta(hours=1), **kwargs)], T, NOW)
    assert d.rearm_ids == [1]
    assert d.low_frames == []
    assert not d.send


def test_null_battery_connected_treated_as_no_battery():
    f = frame(pct=5)
    f["battery_connected"] = None
    d = evaluate([f], T, NOW)
    assert d.low_frames == []
    assert not d.send


def test_null_percent_is_ignored_and_keeps_state():
    d = evaluate([frame(pct=None, alerted=NOW - timedelta(hours=30))], T, NOW)
    assert d.rearm_ids == []
    assert d.low_frames == []
    assert not d.send


def test_reminder_not_due_before_24h():
    d = evaluate([frame(pct=10, alerted=NOW - timedelta(hours=23, minutes=59))], T, NOW)
    assert [f["id"] for f in d.low_frames] == [1]
    assert not d.send


def test_reminder_due_at_24h():
    d = evaluate([frame(pct=10, alerted=NOW - timedelta(hours=24))], T, NOW)
    assert d.send
    assert d.new_ids == set()


# -- evaluate: multiple frames --

def test_digest_includes_all_low_frames_when_one_is_due():
    frames = [
        frame(1, pct=15, mac="AA:01"),                                     # new → due
        frame(2, pct=10, mac="AA:02", alerted=NOW - timedelta(hours=2)),   # not due
        frame(3, pct=80, mac="AA:03"),                                     # healthy
    ]
    d = evaluate(frames, T, NOW)
    assert d.send
    assert [f["id"] for f in d.low_frames] == [2, 1]  # sorted by battery %
    assert d.new_ids == {1}


def test_no_send_when_no_low_frame_is_due():
    frames = [
        frame(1, pct=15, alerted=NOW - timedelta(hours=1)),
        frame(2, pct=10, alerted=NOW - timedelta(hours=5)),
    ]
    assert not evaluate(frames, T, NOW).send


def test_silent_low_frame_still_reminded():
    f = frame(pct=8, alerted=NOW - timedelta(hours=25), last_seen=NOW - timedelta(days=3))
    d = evaluate([f], T, NOW)
    assert d.send
    assert [x["id"] for x in d.low_frames] == [1]


# -- messages --

def test_single_frame_subject_uses_name():
    d = AlertDecision(low_frames=[frame(pct=18, name="Kitchen")], new_ids={1}, send=True)
    msg = notifier.build_alert_message(d, T, CFG, ["me@example.com"])
    assert msg["Subject"] == "Photo frame battery low: Kitchen (18%)"
    assert msg["From"] == "frames@test"
    assert msg["To"] == "me@example.com"


def test_single_frame_subject_falls_back_to_mac():
    d = AlertDecision(low_frames=[frame(pct=18, mac="AA:BB:CC:DD:EE:FF")], send=True)
    msg = notifier.build_alert_message(d, T, CFG, ["me@example.com"])
    assert msg["Subject"] == "Photo frame battery low: AA:BB:CC:DD:EE:FF (18%)"


def test_multi_frame_subject_and_body():
    low = [
        frame(2, pct=10, name="Hall", mv=3500),
        frame(1, pct=15, name="Kitchen", mv=3600),
    ]
    d = AlertDecision(low_frames=low, new_ids={1}, send=True)
    msg = notifier.build_alert_message(d, T, CFG, ["me@example.com", "you@example.org"])
    assert msg["Subject"] == "Photo frame battery low: 2 frames"
    assert msg["To"] == "me@example.com, you@example.org"
    body = msg.get_content()
    hall = body.index("Hall — 10% (3500 mV), last seen 2026-09-24 12:00 UTC")
    kitchen = body.index("Kitchen — 15% (3600 mV), last seen 2026-09-24 12:00 UTC [NEW]")
    assert hall < kitchen
    assert "Hall — 10% (3500 mV), last seen 2026-09-24 12:00 UTC [NEW]" not in body
    assert "20%" in body
    assert "every 24 hours" in body


def test_frame_name_with_newline_is_flattened():
    d = AlertDecision(low_frames=[frame(pct=18, name="Kit\r\nchen")], new_ids={1}, send=True)
    msg = notifier.build_alert_message(d, T, CFG, ["me@example.com"])
    assert msg["Subject"] == "Photo frame battery low: Kit chen (18%)"


def test_test_message():
    msg = notifier.build_test_message(T, CFG, ["me@example.com"])
    assert msg["Subject"] == "Photo frame test email"
    assert msg["To"] == "me@example.com"
    assert "20%" in msg.get_content()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_notifier.py -q`
Expected: collection error `ModuleNotFoundError: No module named 'notifier'`.

- [ ] **Step 3: Create `server/notifier.py`**

```python
"""Low-battery email alerts.

On every frame status report, check_battery_alerts() looks at *all* frames
and sends one combined email when any low frame is due an alert (see
docs/superpowers/specs/2026-09-24-battery-alert-email-design.md).
"""
import logging
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage

import config

logger = logging.getLogger("uvicorn.error")

REARM_MARGIN = 5  # percentage points above the threshold before re-arming
REMINDER_INTERVAL = timedelta(hours=24)


# ── Configuration ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class SmtpConfig:
    host: str
    port: int
    tls: str  # "starttls" or "ssl"
    user: str
    password: str
    sender: str

    @property
    def configured(self) -> bool:
        return bool(self.host and self.sender)


def smtp_config() -> SmtpConfig:
    """Snapshot of the SMTP settings (read at call time)."""
    return SmtpConfig(
        host=config.SMTP_HOST,
        port=config.SMTP_PORT,
        tls=config.SMTP_TLS,
        user=config.SMTP_USER,
        password=config.SMTP_PASSWORD,
        sender=config.SMTP_FROM,
    )


def parse_recipients(value: str) -> list[str] | None:
    """Split a comma-separated recipient list.

    Returns [] when empty (alerts off) and None when any address is invalid.
    Rejecting whitespace (including CR/LF) also blocks header injection.
    """
    recipients = [part.strip() for part in value.split(",") if part.strip()]
    for addr in recipients:
        if (addr.count("@") != 1 or addr.startswith("@") or addr.endswith("@")
                or any(ch.isspace() for ch in addr)):
            return None
    return recipients


# ── Alert rules ──────────────────────────────────────────────────────────

@dataclass
class AlertDecision:
    rearm_ids: list[int] = field(default_factory=list)
    low_frames: list[dict] = field(default_factory=list)
    new_ids: set[int] = field(default_factory=set)
    send: bool = False


def evaluate(frames: list[dict], threshold: int, now: datetime) -> AlertDecision:
    """Decide which frames re-arm, which are low, and whether to send."""
    decision = AlertDecision()
    for f in frames:
        pct = f["battery_percent"]
        alerted = f["low_battery_alerted_at"]
        on_power = bool(f["charging"]) or bool(f["usb_connected"])

        if (not f["battery_connected"] or on_power
                or (pct is not None and pct >= threshold + REARM_MARGIN)):
            if alerted is not None:
                decision.rearm_ids.append(f["id"])
            continue

        if pct is None or pct >= threshold:
            continue  # not low; alert state unchanged (hysteresis band)

        decision.low_frames.append(f)
        if alerted is None:
            decision.new_ids.add(f["id"])
            decision.send = True
        elif now - datetime.fromisoformat(alerted) >= REMINDER_INTERVAL:
            decision.send = True

    decision.low_frames.sort(key=lambda f: f["battery_percent"])
    return decision


# ── Messages ─────────────────────────────────────────────────────────────

def _frame_label(f: dict) -> str:
    # Collapse whitespace so a name can never inject header lines.
    return " ".join((f["name"] or f["mac_address"]).split())


def _format_last_seen(value: str | None) -> str:
    if not value:
        return "never"
    return datetime.fromisoformat(value).astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def _message(cfg: SmtpConfig, recipients: list[str], subject: str, body: str) -> EmailMessage:
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = cfg.sender
    msg["To"] = ", ".join(recipients)
    msg.set_content(body)
    return msg


def build_alert_message(decision: AlertDecision, threshold: int,
                        cfg: SmtpConfig, recipients: list[str]) -> EmailMessage:
    low = decision.low_frames
    if len(low) == 1:
        subject = (f"Photo frame battery low: {_frame_label(low[0])} "
                   f"({low[0]['battery_percent']}%)")
    else:
        subject = f"Photo frame battery low: {len(low)} frames"

    lines = []
    for f in low:
        mv = f" ({f['battery_mv']} mV)" if f["battery_mv"] is not None else ""
        line = (f"{_frame_label(f)} — {f['battery_percent']}%{mv}, "
                f"last seen {_format_last_seen(f['last_seen'])}")
        if f["id"] in decision.new_ids:
            line += " [NEW]"
        lines.append(line)

    body = (
        f"These photo frames are below the {threshold}% battery alert threshold:\n\n"
        + "\n".join(lines)
        + "\n\nYou'll get a reminder every 24 hours until these frames are charged.\n"
    )
    return _message(cfg, recipients, subject, body)


def build_test_message(threshold: int, cfg: SmtpConfig, recipients: list[str]) -> EmailMessage:
    body = (
        "This is a test email from your photo frame server.\n\n"
        "Your SMTP settings work. Low-battery alerts will be sent to this "
        f"address when a frame drops below {threshold}%.\n"
    )
    return _message(cfg, recipients, "Photo frame test email", body)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

- [ ] **Step 5: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add notifier.py tests/test_notifier.py
git commit -m "feat(server): add low-battery alert rules and email message builders" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 5: Notifier — SMTP sending and the alert check

**Files:**
- Modify: `server/notifier.py` (add imports; add `send_email`, `_lock`, `check_battery_alerts`, `_check`)
- Modify: `server/tests/conftest.py` (add lock-reset fixture)
- Test: `server/tests/test_notifier.py` (append)

**Interfaces:**
- Consumes: Task 3 DB methods; Task 4 functions.
- Produces:
  - `send_email(cfg: SmtpConfig, msg: EmailMessage) -> None` (synchronous; raises on any SMTP or network error)
  - `async check_battery_alerts(db, now: datetime | None = None) -> None` (never raises)
  - Module attribute `_lock: asyncio.Lock`
  - Test pattern: monkeypatch `notifier.send_email` (looked up as a module global at call time, so patching the module attribute takes effect everywhere, including `main.py`, which calls `notifier.send_email`).

- [ ] **Step 1: Add the lock-reset fixture to `server/tests/conftest.py`**

Add `import asyncio` and `import pytest` to the imports, then append:

```python


@pytest.fixture(autouse=True)
def _fresh_notifier_lock():
    """Give each test a new alert lock.

    pytest-asyncio uses a fresh event loop per test, and an asyncio.Lock that
    was ever contended stays bound to the loop it first waited on.
    """
    import notifier
    notifier._lock = asyncio.Lock()
```

(`import notifier` stays inside the fixture so that it runs after the env setup at the top of conftest.)

- [ ] **Step 2: Write the failing tests**

Append to `server/tests/test_notifier.py` (add `import smtplib` and `import time` to the imports, and `from database import Database` after `import notifier`):

```python
# ── send_email ───────────────────────────────────────────────────────────

class FakeSMTP:
    instances: list["FakeSMTP"] = []

    def __init__(self, host, port, timeout=None, context=None):
        self.host, self.port, self.timeout = host, port, timeout
        self.calls = []
        FakeSMTP.instances.append(self)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.calls.append("quit")
        return False

    def starttls(self, context=None):
        self.calls.append("starttls")

    def login(self, user, password):
        self.calls.append(("login", user, password))

    def send_message(self, msg):
        self.calls.append(("send", msg["Subject"]))


@pytest.fixture
def fake_smtp(monkeypatch):
    FakeSMTP.instances = []
    monkeypatch.setattr(notifier.smtplib, "SMTP", FakeSMTP)
    monkeypatch.setattr(notifier.smtplib, "SMTP_SSL", FakeSMTP)
    return FakeSMTP


def test_send_email_starttls_with_login(fake_smtp):
    notifier.send_email(CFG, notifier.build_test_message(T, CFG, ["me@example.com"]))
    (smtp,) = fake_smtp.instances
    assert (smtp.host, smtp.port, smtp.timeout) == ("smtp.test", 587, 15)
    assert smtp.calls == [
        "starttls", ("login", "user@test", "pw"), ("send", "Photo frame test email"), "quit",
    ]


def test_send_email_without_user_skips_login(fake_smtp):
    cfg = SmtpConfig("smtp.test", 25, "starttls", "", "", "frames@test")
    notifier.send_email(cfg, notifier.build_test_message(T, cfg, ["me@example.com"]))
    assert not any(isinstance(c, tuple) and c[0] == "login" for c in fake_smtp.instances[0].calls)


def test_send_email_ssl_uses_smtp_ssl_without_starttls(monkeypatch, fake_smtp):
    used = []

    class FakeSSL(FakeSMTP):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            used.append("ssl")

    monkeypatch.setattr(notifier.smtplib, "SMTP_SSL", FakeSSL)
    cfg = SmtpConfig("smtp.test", 465, "ssl", "user@test", "pw", "frames@test")
    notifier.send_email(cfg, notifier.build_test_message(T, cfg, ["me@example.com"]))
    assert used == ["ssl"]
    assert "starttls" not in fake_smtp.instances[0].calls


# ── check_battery_alerts (with a real database) ──────────────────────────

@pytest.fixture
async def db(tmp_path):
    d = Database(tmp_path / "test.db")
    await d.init()
    yield d
    await d.close()


@pytest.fixture
def sent(monkeypatch):
    """Enable SMTP config and capture sent messages instead of sending."""
    import config
    monkeypatch.setattr(config, "SMTP_HOST", "smtp.test")
    monkeypatch.setattr(config, "SMTP_FROM", "frames@test")
    messages = []
    monkeypatch.setattr(notifier, "send_email", lambda cfg, msg: messages.append(msg))
    return messages


async def add_frame(db, mac, pct, *, charging=False, usb=False, connected=True, name=None):
    frame_id = await db.get_or_create_frame(mac, "k")
    await db.update_frame_status(frame_id, {
        "battery_connected": connected, "battery_percent": pct, "battery_mv": 3600,
        "charging": charging, "usb_connected": usb,
    })
    if name:
        await db.update_frame_name(frame_id, name)
    return frame_id


async def alerted_at(db, frame_id):
    return (await db.get_frame(frame_id))["low_battery_alerted_at"]


async def test_check_does_nothing_without_smtp_host(db, sent, monkeypatch):
    import config
    monkeypatch.setattr(config, "SMTP_HOST", "")
    await db.set_alert_settings("me@example.com", T)
    await add_frame(db, "AA:01", 5)
    await notifier.check_battery_alerts(db, now=NOW)
    assert sent == []


async def test_check_does_nothing_without_recipient(db, sent):
    await add_frame(db, "AA:01", 5)
    await notifier.check_battery_alerts(db, now=NOW)
    assert sent == []


async def test_check_sends_once_and_records(db, sent):
    await db.set_alert_settings("me@example.com", T)
    fid = await add_frame(db, "AA:01", 15, name="Kitchen")

    await notifier.check_battery_alerts(db, now=NOW)
    assert len(sent) == 1
    assert sent[0]["Subject"] == "Photo frame battery low: Kitchen (15%)"
    assert sent[0]["To"] == "me@example.com"
    assert await alerted_at(db, fid) == NOW.isoformat()

    await notifier.check_battery_alerts(db, now=NOW + timedelta(hours=1))
    assert len(sent) == 1


async def test_check_sends_reminder_after_24h(db, sent):
    await db.set_alert_settings("me@example.com", T)
    fid = await add_frame(db, "AA:01", 15)
    await notifier.check_battery_alerts(db, now=NOW)
    later = NOW + timedelta(hours=24)
    await notifier.check_battery_alerts(db, now=later)
    assert len(sent) == 2
    assert "[NEW]" not in sent[1].get_content()
    assert await alerted_at(db, fid) == later.isoformat()


async def test_check_rearms_after_charging_then_alerts_again(db, sent):
    await db.set_alert_settings("me@example.com", T)
    fid = await add_frame(db, "AA:01", 15)
    await notifier.check_battery_alerts(db, now=NOW)

    await add_frame(db, "AA:01", 16, charging=True)
    await notifier.check_battery_alerts(db, now=NOW + timedelta(hours=1))
    assert await alerted_at(db, fid) is None
    assert len(sent) == 1

    await add_frame(db, "AA:01", 12)
    await notifier.check_battery_alerts(db, now=NOW + timedelta(hours=2))
    assert len(sent) == 2
    assert "[NEW]" in sent[1].get_content()


async def test_check_digest_lists_all_low_frames_and_records_all(db, sent):
    await db.set_alert_settings("me@example.com", T)
    a = await add_frame(db, "AA:01", 15, name="Kitchen")
    await notifier.check_battery_alerts(db, now=NOW)

    b = await add_frame(db, "AA:02", 10, name="Hall")
    later = NOW + timedelta(hours=2)
    await notifier.check_battery_alerts(db, now=later)

    assert len(sent) == 2
    assert sent[1]["Subject"] == "Photo frame battery low: 2 frames"
    body = sent[1].get_content()
    assert "Hall — 10%" in body and "Kitchen — 15%" in body
    assert await alerted_at(db, a) == later.isoformat()
    assert await alerted_at(db, b) == later.isoformat()


async def test_check_send_failure_records_nothing_then_retries(db, sent, monkeypatch):
    await db.set_alert_settings("me@example.com", T)
    fid = await add_frame(db, "AA:01", 15)

    def boom(cfg, msg):
        raise smtplib.SMTPServerDisconnected("down")

    monkeypatch.setattr(notifier, "send_email", boom)
    await notifier.check_battery_alerts(db, now=NOW)  # must not raise
    assert await alerted_at(db, fid) is None

    monkeypatch.setattr(notifier, "send_email", lambda cfg, msg: sent.append(msg))
    await notifier.check_battery_alerts(db, now=NOW + timedelta(hours=1))
    assert len(sent) == 1


async def test_check_rearms_persist_even_if_send_fails(db, sent, monkeypatch):
    await db.set_alert_settings("me@example.com", T)
    a = await add_frame(db, "AA:01", 15)
    await notifier.check_battery_alerts(db, now=NOW)

    await add_frame(db, "AA:01", 15, charging=True)   # a re-arms
    b = await add_frame(db, "AA:02", 10)               # b is new → send attempted

    def boom(cfg, msg):
        raise OSError("network unreachable")

    monkeypatch.setattr(notifier, "send_email", boom)
    await notifier.check_battery_alerts(db, now=NOW + timedelta(hours=1))
    assert await alerted_at(db, a) is None
    assert await alerted_at(db, b) is None


async def test_concurrent_checks_send_only_once(db, sent, monkeypatch):
    import asyncio
    await db.set_alert_settings("me@example.com", T)
    await add_frame(db, "AA:01", 15)

    def slow_send(cfg, msg):
        time.sleep(0.2)  # runs in a worker thread; widens the race window
        sent.append(msg)

    monkeypatch.setattr(notifier, "send_email", slow_send)
    await asyncio.gather(
        notifier.check_battery_alerts(db, now=NOW),
        notifier.check_battery_alerts(db, now=NOW),
    )
    assert len(sent) == 1


async def test_check_swallows_unexpected_errors(db, sent, monkeypatch):
    await db.set_alert_settings("me@example.com", T)

    async def broken():
        raise RuntimeError("db exploded")

    monkeypatch.setattr(db, "list_frames", broken)
    await notifier.check_battery_alerts(db, now=NOW)  # must not raise
    assert sent == []
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_notifier.py -q`
Expected: the new tests FAIL with `AttributeError` (`notifier` has no `smtplib`, `send_email` or `check_battery_alerts`). The Task 4 tests still pass: the conftest fixture only assigns `notifier._lock`, which works before the implementation exists.

- [ ] **Step 4: Implement in `server/notifier.py`**

Replace the import block with:

```python
import asyncio
import logging
import smtplib
import ssl
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage

import config
```

After the `REMINDER_INTERVAL` constant, add:

```python
SMTP_TIMEOUT = 15  # seconds

# Serialises check → send → record so two simultaneous frame reports can't
# both send the same alert. The server is a single uvicorn process.
_lock = asyncio.Lock()
```

Append to the end of the file:

```python


# ── Sending ──────────────────────────────────────────────────────────────

def send_email(cfg: SmtpConfig, msg: EmailMessage) -> None:
    """Send synchronously (call via asyncio.to_thread). Raises on failure."""
    context = ssl.create_default_context()
    if cfg.tls == "ssl":
        smtp = smtplib.SMTP_SSL(cfg.host, cfg.port, timeout=SMTP_TIMEOUT, context=context)
    else:
        smtp = smtplib.SMTP(cfg.host, cfg.port, timeout=SMTP_TIMEOUT)
    with smtp:
        if cfg.tls != "ssl":
            smtp.starttls(context=context)
        if cfg.user:
            smtp.login(cfg.user, cfg.password)
        smtp.send_message(msg)


# ── Check (runs as a background task after each status report) ───────────

async def check_battery_alerts(db, now: datetime | None = None) -> None:
    """Evaluate all frames and email if any low frame is due. Never raises."""
    try:
        async with _lock:
            await _check(db, now or datetime.now(timezone.utc))
    except Exception:
        logger.exception("Battery alert check failed")


async def _check(db, now: datetime) -> None:
    cfg = smtp_config()
    settings = await db.get_alert_settings()
    recipients = parse_recipients(settings["email"]) or []
    if not cfg.configured or not recipients:
        return

    decision = evaluate(await db.list_frames(), settings["threshold"], now)
    if decision.rearm_ids:
        await db.clear_low_battery_alerts(decision.rearm_ids)
    if not decision.send:
        return

    msg = build_alert_message(decision, settings["threshold"], cfg, recipients)
    try:
        await asyncio.to_thread(send_email, cfg, msg)
    except Exception:
        # Nothing recorded, so the next status report retries.
        logger.exception("Failed to send low-battery alert email")
        return

    await db.set_low_battery_alerted([f["id"] for f in decision.low_frames], now.isoformat())
    logger.info("Sent low-battery alert for %d frame(s)", len(decision.low_frames))
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

To check that the lock test is meaningful: temporarily change `async with _lock:` to `if True:` and run `pytest tests/test_notifier.py::test_concurrent_checks_send_only_once -q`. Expected: FAIL (2 sends). Then revert the change.

- [ ] **Step 6: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add notifier.py tests/conftest.py tests/test_notifier.py
git commit -m "feat(server): send low-battery digest email via SMTP with dedupe lock" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 6: Trigger the check from `/api/status`

**Files:**
- Modify: `server/main.py` (imports lines 1–22; `api_status` ~line 118)
- Test: `server/tests/test_api.py` (append a new section before `# ── Health`)

**Interfaces:**
- Consumes: `notifier.check_battery_alerts(db)`, `notifier.send_email` (monkeypatched in tests), `db.set_alert_settings`.
- Produces: the `alerts_enabled` fixture in `test_api.py` (reused by Task 7), which returns the list of captured `EmailMessage`s.

Note: with httpx `ASGITransport`, `await client.post(...)` returns only after Starlette has run the response's background tasks, so assertions right after the request are deterministic.

- [ ] **Step 1: Write the failing tests**

In `server/tests/test_api.py`, add `import notifier` after `import config`. Then insert before `# ── Health`:

```python
# ── Low-battery alerts ──────────────────────────────────────────────────

LOW_STATUS = {
    "battery_connected": True, "battery_percent": 12, "battery_mv": 3550,
    "charging": False, "usb_connected": False,
}


@pytest.fixture
async def alerts_enabled(monkeypatch):
    """SMTP configured + recipient saved; returns the list of 'sent' messages."""
    monkeypatch.setattr(config, "SMTP_HOST", "smtp.test")
    monkeypatch.setattr(config, "SMTP_FROM", "frames@test")
    messages = []
    monkeypatch.setattr(notifier, "send_email", lambda cfg, msg: messages.append(msg))
    await db.set_alert_settings("me@example.com", 20)
    return messages


@pytest.mark.asyncio
async def test_low_status_report_sends_alert_email(alerts_enabled):
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post("/api/status", headers=HEADERS, json=LOW_STATUS)
    assert r.status_code == 200
    assert len(alerts_enabled) == 1
    assert alerts_enabled[0]["Subject"] == "Photo frame battery low: AA:BB:CC:DD:EE:FF (12%)"
    assert alerts_enabled[0]["To"] == "me@example.com"


@pytest.mark.asyncio
async def test_repeat_low_status_report_does_not_resend(alerts_enabled):
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        await client.post("/api/status", headers=HEADERS, json=LOW_STATUS)
        await client.post("/api/status", headers=HEADERS, json=LOW_STATUS)
    assert len(alerts_enabled) == 1


@pytest.mark.asyncio
async def test_healthy_status_report_sends_nothing(alerts_enabled):
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        await client.post("/api/status", headers=HEADERS,
                          json={**LOW_STATUS, "battery_percent": 80})
    assert alerts_enabled == []


@pytest.mark.asyncio
async def test_status_report_succeeds_when_alert_check_errors(alerts_enabled, monkeypatch):
    async def broken():
        raise RuntimeError("settings unavailable")

    monkeypatch.setattr(db, "get_alert_settings", broken)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post("/api/status", headers=HEADERS, json=LOW_STATUS)
    assert r.status_code == 200
    assert alerts_enabled == []
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_api.py -q`
Expected: `test_low_status_report_sends_alert_email` and `test_repeat_low_status_report_does_not_resend` FAIL (`len == 0`). The other two pass trivially for now.

- [ ] **Step 3: Implement in `server/main.py`**

(a) Imports. Add `from urllib.parse import quote` after `from pathlib import Path`. Add `BackgroundTasks,` as the first name inside the `from fastapi import (...)` list. Add `import notifier` after `import config`. The top of the file becomes:

```python
import io
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import quote

from fastapi import (
    BackgroundTasks,
    Depends,
    FastAPI,
    File,
    Form,
    Header,
    HTTPException,
    Request,
    UploadFile,
)
from fastapi.responses import FileResponse, RedirectResponse, Response
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from PIL import Image
from pydantic import BaseModel

import config
import notifier
from database import Database
```

Then delete the two now-redundant function-local `from urllib.parse import quote` lines (in `save_frame_settings` and `save_settings`). Ruff doesn't flag them, but they're redundant once the module-level import exists.

(b) Replace `api_status` with:

```python
@app.post("/api/status")
async def api_status(status: FrameStatus, background_tasks: BackgroundTasks,
                     frame_id: int = Depends(get_frame_id)):
    await db.update_frame_status(frame_id, status.model_dump())
    # Runs after the response is sent, so it never extends the frame's awake time.
    background_tasks.add_task(notifier.check_battery_alerts, db)
    return {"ok": True}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

- [ ] **Step 5: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add main.py tests/test_api.py
git commit -m "feat(server): check battery alerts after each frame status report" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 7: Web UI — alert settings card, save and test-email routes

**Files:**
- Modify: `server/main.py` (`index` ~line 233; new routes after `save_settings` ~line 367)
- Modify: `server/templates/index.html` (toasts ~line 14; settings section ~line 86–104)
- Modify: `server/static/style.css` (Settings block ~line 360; after `.settings-hint` ~line 847)
- Test: `server/tests/test_api.py` (append to the alerts section)

**Interfaces:**
- Consumes: `notifier.parse_recipients`, `notifier.smtp_config`, `notifier.build_test_message`, `notifier.send_email`, `db.get_alert_settings`, `db.set_alert_settings`, and the `alerts_enabled` fixture (Task 6).
- Produces: `POST /settings/alerts` (form fields `alert_email`, `battery_alert_threshold`), `POST /settings/alerts/test`, the `GET /?notice=...` toast, and template context `alerts: {"email", "threshold"}` and `smtp_configured: bool`.

- [ ] **Step 1: Write the failing tests**

Add `from urllib.parse import unquote` to the imports at the top of `server/tests/test_api.py`, and `import smtplib` too. Append to the alerts section (before `# ── Health`):

```python
@pytest.mark.asyncio
async def test_save_alert_settings_normalises_and_stores():
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts", data={
            "alert_email": " me@example.com ,you@example.org ",
            "battery_alert_threshold": "15",
        })
    assert r.status_code == 303
    assert r.headers["location"] == "/?saved=1"
    assert await db.get_alert_settings() == {
        "email": "me@example.com, you@example.org", "threshold": 15,
    }


@pytest.mark.asyncio
async def test_save_alert_settings_empty_email_disables():
    await db.set_alert_settings("me@example.com", 20)
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts", data={
            "alert_email": "", "battery_alert_threshold": "20",
        })
    assert r.headers["location"] == "/?saved=1"
    assert (await db.get_alert_settings())["email"] == ""


@pytest.mark.asyncio
@pytest.mark.parametrize("email,threshold", [
    ("not-an-email", "20"),
    ("me@example.com\r\nBcc: evil@example.com", "20"),
    ("me@example.com", "0"),
    ("me@example.com", "100"),
    ("me@example.com", "abc"),
    ("me@example.com", ""),
])
async def test_save_alert_settings_rejects_invalid(email, threshold):
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts", data={
            "alert_email": email, "battery_alert_threshold": threshold,
        })
    assert r.status_code == 303
    assert "error=" in r.headers["location"]
    assert await db.get_alert_settings() == {"email": "", "threshold": 20}


@pytest.mark.asyncio
async def test_test_email_success(alerts_enabled):
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts/test")
    assert r.status_code == 303
    location = unquote(r.headers["location"])
    assert "notice=Test email sent to me@example.com" in location
    assert [m["Subject"] for m in alerts_enabled] == ["Photo frame test email"]


@pytest.mark.asyncio
async def test_test_email_smtp_failure_shows_error(alerts_enabled, monkeypatch):
    def boom(cfg, msg):
        raise smtplib.SMTPAuthenticationError(535, b"bad credentials")

    monkeypatch.setattr(notifier, "send_email", boom)
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts/test")
    location = unquote(r.headers["location"])
    assert "error=Test email failed:" in location
    assert "bad credentials" in location


@pytest.mark.asyncio
async def test_test_email_requires_smtp(monkeypatch):
    monkeypatch.setattr(config, "SMTP_HOST", "")
    await db.set_alert_settings("me@example.com", 20)
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts/test")
    assert "SMTP not configured" in unquote(r.headers["location"])


@pytest.mark.asyncio
async def test_test_email_requires_recipient(monkeypatch):
    monkeypatch.setattr(config, "SMTP_HOST", "smtp.test")
    monkeypatch.setattr(config, "SMTP_FROM", "frames@test")
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts/test")
    assert "No alert recipient saved" in unquote(r.headers["location"])


@pytest.mark.asyncio
async def test_dashboard_hint_smtp_not_configured(monkeypatch):
    monkeypatch.setattr(config, "SMTP_HOST", "")
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/")
    assert "SMTP not configured. Set PHOTOFRAME_SMTP_HOST (and SMTP_USER or SMTP_FROM) in .env." in r.text


@pytest.mark.asyncio
async def test_dashboard_hint_no_recipient(monkeypatch):
    monkeypatch.setattr(config, "SMTP_HOST", "smtp.test")
    monkeypatch.setattr(config, "SMTP_FROM", "frames@test")
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/")
    assert "Alerts off: no recipient set." in r.text


@pytest.mark.asyncio
async def test_dashboard_hint_alerts_on(alerts_enabled):
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/")
    assert "Alerts on: emailing me@example.com when a frame drops below 20%." in r.text
    assert 'name="alert_email" value="me@example.com"' in r.text


@pytest.mark.asyncio
async def test_dashboard_shows_notice_toast():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/", params={"notice": "Hello there"})
    assert '<div class="toast">Hello there</div>' in r.text
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `venv/Scripts/python -m pytest tests/test_api.py -q`
Expected: the new tests FAIL (404 for the `/settings/alerts*` routes; missing hint text and toast).

- [ ] **Step 3: Implement the routes and context in `server/main.py`**

(a) Add `import asyncio` as the first line of the file (used by the test-email route below). Then replace the `index` function with:

```python
@app.get("/")
async def index(request: Request, saved: int | None = None, error: str | None = None,
                notice: str | None = None):
    frames = await db.list_frames()
    # Add image count per frame (single query).
    counts = await db.get_frame_image_counts()
    for frame in frames:
        frame["image_count"] = counts.get(frame["id"], 0)
    wake = await db.get_wake_interval()
    alerts = await db.get_alert_settings()
    nav = await _nav_context()
    return templates.TemplateResponse(request, "index.html", context={
        **nav,
        "frames": frames,
        "wake": wake,
        "alerts": alerts,
        "smtp_configured": notifier.smtp_config().configured,
        "saved": saved,
        "error": error,
        "notice": notice,
    })
```

(b) After `save_settings`, before `# ── Web UI: Logs`, add the following (the dashboard hint is the only place the longer "SMTP not configured" wording appears; the redirect error is intentionally shorter):

```python


# ── Web UI: Battery alerts ─────────────────────────────────────────────

def _validate_alert_settings(form) -> tuple[str, int] | str:
    """Validate alert form data. Returns (email, threshold) or error string."""
    recipients = notifier.parse_recipients(str(form.get("alert_email", "")))
    if recipients is None:
        return "Invalid email address"
    try:
        threshold = int(form.get("battery_alert_threshold", ""))
    except (ValueError, TypeError):
        return "Invalid battery threshold"
    if not (1 <= threshold <= 99):
        return "Battery threshold must be 1–99%"
    return (", ".join(recipients), threshold)


@app.post("/settings/alerts")
async def save_alert_settings(request: Request):
    form = await request.form()
    result = _validate_alert_settings(form)
    if isinstance(result, str):
        return RedirectResponse(url=f"/?error={quote(result)}", status_code=303)
    email, threshold = result
    await db.set_alert_settings(email, threshold)
    return RedirectResponse(url="/?saved=1", status_code=303)


@app.post("/settings/alerts/test")
async def send_test_alert():
    cfg = notifier.smtp_config()
    settings = await db.get_alert_settings()
    recipients = notifier.parse_recipients(settings["email"]) or []
    if not cfg.configured:
        error = "SMTP not configured. Set PHOTOFRAME_SMTP_HOST in .env."
    elif not recipients:
        error = "No alert recipient saved"
    else:
        msg = notifier.build_test_message(settings["threshold"], cfg, recipients)
        try:
            # Synchronous from the user's view, so SMTP errors show immediately.
            await asyncio.to_thread(notifier.send_email, cfg, msg)
        except Exception as e:
            error = f"Test email failed: {e}"
        else:
            notice = f"Test email sent to {settings['email']}"
            return RedirectResponse(url=f"/?notice={quote(notice)}", status_code=303)
    return RedirectResponse(url=f"/?error={quote(error)}", status_code=303)
```

- [ ] **Step 4: Update `server/templates/index.html`**

(a) After the `{% if error %}...{% endif %}` toast block (~line 19), add:

```html
        {% if notice %}
        <div class="toast">{{ notice }}</div>
        {% endif %}
```

(b) After the closing `</div>` of the existing wake-interval `settings-card` (the one ending with the `settings-hint` "Applies to frames without a custom wake interval."), and before `</section>`, add:

```html
            <div class="settings-card">
                <form action="/settings/alerts" method="post" class="settings-form">
                    <span class="settings-label">Battery alerts</span>
                    <input type="text" name="alert_email" value="{{ alerts.email }}"
                           placeholder="you@example.com" class="text-input alert-email-input"
                           aria-label="Alert email address">
                    <div class="time-group">
                        <input type="number" name="battery_alert_threshold" value="{{ alerts.threshold }}"
                               min="1" max="99" class="time-input" aria-label="Battery alert threshold">
                        <span class="time-unit">%</span>
                    </div>
                    <button type="submit" class="btn btn-save">Save</button>
                </form>
                <form action="/settings/alerts/test" method="post" class="alert-test-form">
                    <button type="submit" class="btn btn-primary">Send test email</button>
                </form>
                <p class="settings-hint">
                    {% if not smtp_configured %}SMTP not configured. Set PHOTOFRAME_SMTP_HOST (and SMTP_USER or SMTP_FROM) in .env.
                    {% elif not alerts.email %}Alerts off: no recipient set.
                    {% else %}Alerts on: emailing {{ alerts.email }} when a frame drops below {{ alerts.threshold }}%.
                    {% endif %}
                </p>
            </div>
```

- [ ] **Step 5: Add styles to `server/static/style.css`**

After the `.settings-card { ... }` rule (~line 365), add:

```css
.settings-card + .settings-card {
  margin-top: 1rem;
}
```

After the `.settings-hint { ... }` rule (~line 847), add:

```css
.alert-email-input {
  width: 18rem;
  max-width: 100%;
}

.alert-test-form {
  margin-top: 0.75rem;
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `venv/Scripts/python -m pytest -q`
Expected: all pass.

- [ ] **Step 7: Visual check**

Start the server natively against a scratch data dir:

```bash
PHOTOFRAME_DATA_DIR="$(mktemp -d)" PHOTOFRAME_API_KEY=dev venv/Scripts/python -m uvicorn main:app --port 8080
```

Open `http://localhost:8080/`. Check that the **Battery alerts** card sits below the wake-interval card with a gap, that the inputs line up with the wake inputs, and that it wraps sensibly at phone width (about 400px). Check that saving an invalid email shows the red error toast, and that **Send test email** shows the "SMTP not configured" error. Stop the server.

- [ ] **Step 8: Lint and commit**

```bash
venv/Scripts/python -m ruff check .
git add main.py templates/index.html static/style.css tests/test_api.py
git commit -m "feat(server): dashboard card for battery alert settings and test email" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 8: Documentation

**Files:**
- Modify: `DECISIONS.md` (append ADR-022)
- Modify: `README.md` (Features list; Option A server setup; new "Battery alerts" subsection before `### Server development on Windows`)
- Modify: `CLAUDE.md` (Server Architecture bullets)
- Modify: `TODO.md` (Phase 10 section)
- Modify: `PROGRESS.md` (new entry at top)

- [ ] **Step 1: Append ADR-022 to `DECISIONS.md`**

```markdown

## ADR-022 — Low-battery email alerts via SMTP, driven by status reports

**Status:** Accepted
**Date:** 2026-09-24

**Decision:** The server emails a configurable recipient when any frame's
battery drops below a threshold (default 20%). The check runs as a FastAPI
background task after every `POST /api/status` and evaluates **all** frames,
sending one combined email. A frame re-arms once it is charging, on USB, has
no battery, or reaches threshold + 5%. While any low frame remains, a reminder
goes out every 24h. Each frame's alert state is a `frames.low_battery_alerted_at`
timestamp, written only after a successful send, and an in-process
`asyncio.Lock` prevents duplicate sends. SMTP credentials come from `.env`
(`PHOTOFRAME_SMTP_*`); the recipient and threshold are dashboard settings.

**Rationale:** Stdlib `smtplib` adds no dependencies on the Pi Zero 2W, and
works with Gmail app passwords or any relay. Driving the check from status
reports avoids a scheduler: frames already report on every wake. Running it
after the response means SMTP latency never extends the frame's awake time,
which matters for battery life. One digest per event, with a shared 24h cycle,
keeps multi-frame households to one email a day. A frame that dies while low
keeps appearing in reminders because other frames' reports drive the check.
That is intentional, since a silent low frame is almost certainly flat.
```

- [ ] **Step 2: Update `README.md`**

(a) In `## Features`, after the **Remote monitoring** bullet, add:

```markdown
- **Low-battery email alerts** — one digest email when any frame drops below a threshold, with daily reminders until charged
```

(b) Immediately before `### Server development on Windows`, add:

````markdown
### Battery alert emails (optional)

The server can email you when any frame's battery drops below a threshold.

1. Add SMTP settings to `server/.env` (see `.env.example`). For example, Gmail
   with an [app password](https://myaccount.google.com/apppasswords):
   ```bash
   PHOTOFRAME_SMTP_HOST=smtp.gmail.com
   PHOTOFRAME_SMTP_PORT=587
   PHOTOFRAME_SMTP_TLS=starttls
   PHOTOFRAME_SMTP_USER=you@gmail.com
   PHOTOFRAME_SMTP_PASSWORD=your-app-password
   ```
   Then run `docker compose up -d` to restart with the new settings.
2. On the dashboard, under **Global Settings → Battery alerts**, enter the
   recipient address (comma-separate several) and the threshold (default 20%),
   then click **Save**. Use **Send test email** to check your settings.

You get one email listing every low frame, then a reminder every 24 hours
until they're charged. A frame re-arms once it's charging, on USB, or back
above the threshold + 5%.
````

- [ ] **Step 3: Update `CLAUDE.md`**

In `### Server Architecture`, after the `- **Multi-frame**: ...` bullet, add:

```markdown
- **Battery alerts**: `notifier.py` — after each `/api/status`, a background task checks *all* frames and sends one SMTP digest email (stdlib `smtplib`). SMTP creds in `.env` (`PHOTOFRAME_SMTP_*`); recipient + threshold in the `settings` table via the dashboard. State: `frames.low_battery_alerted_at`. See ADR-022
- **Tests**: `tests/conftest.py` points `PHOTOFRAME_DATA_DIR` at a temp dir — `test_api.py` wipes the DB/images between tests, so never bypass it
```

- [ ] **Step 4: Update `TODO.md`**

In `## Phase 10 — Power Optimisation`, after the `- [ ] Low-battery warning — display message on EPD ...` line, add:

```markdown
- [x] Low-battery email alert — server emails a digest when any frame drops below a threshold (ADR-022)
```

- [ ] **Step 5: Add an entry at the top of `PROGRESS.md`**

Insert after the `# Progress Log` heading line:

```markdown

## 2026-09-24 — Low-Battery Email Alerts

- **Email alerts**: server emails a single digest when any frame's battery drops
  below a threshold (default 20%), then a reminder every 24h until charged
- **Re-arm hysteresis**: alert re-arms on charging/USB/no battery or at threshold + 5%
- **SMTP via stdlib**: `PHOTOFRAME_SMTP_*` in `.env`; no new dependencies
- **Dashboard card**: recipient + threshold settings and a "Send test email" button
- **Background task**: check runs after the `/api/status` response, so a frame's awake time is unaffected
- **Test isolation**: suite now runs in a temp data dir (previously wiped `server/photoframe.db`)
- No firmware changes needed.

---
```

- [ ] **Step 6: Commit**

```bash
git add ../DECISIONS.md ../README.md ../CLAUDE.md ../TODO.md ../PROGRESS.md
git commit -m "docs: document low-battery email alerts (ADR-022)" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013roeWBNkMkFCSi6gTnAbAw"
```

---

### Task 9: Final verification

- [ ] **Step 1: Full test suite and lint**

```bash
venv/Scripts/python -m pytest -q
venv/Scripts/python -m ruff check .
```
Expected: all tests pass (26 at baseline, plus the new ones); ruff reports `All checks passed!`

- [ ] **Step 2: Confirm the real data dir is untouched by tests**

Run `ls -la photoframe.db images thumbs` before and after `pytest -q`. The modification times must be unchanged.

- [ ] **Step 3: Container build smoke test (if Docker is available locally)**

```bash
docker build -t photoframe-alerts-test .
docker run --rm -d --name pf-alerts -p 8081:8080 -e PHOTOFRAME_API_KEY=dev photoframe-alerts-test
curl -fsS http://localhost:8081/healthz
curl -fsS http://localhost:8081/ | grep -c "Battery alerts"
docker stop pf-alerts
```
Expected: `{"status":"ok"}` and a count of `1` or more. If Docker isn't available, note that CI's smoke boot covers this on push.

- [ ] **Step 4: Real SMTP check (manual, by the owner)**

With real `PHOTOFRAME_SMTP_*` values in `.env`, set the recipient on the dashboard and click **Send test email**. Expected: the success toast appears and the email arrives. The owner does this step on the Pi after merge; it's listed here so it isn't forgotten.

- [ ] **Step 5: Pre-merge reviews (required by CLAUDE.md)**

Before merging the PR, run all three and fix what they find:
1. `/code-review` on the branch diff.
2. Document review of `README.md`, `CLAUDE.md`, `DECISIONS.md`, `.env.example` and the spec, checking accuracy and consistency.
3. `claude-md-management:claude-md-improver`.
