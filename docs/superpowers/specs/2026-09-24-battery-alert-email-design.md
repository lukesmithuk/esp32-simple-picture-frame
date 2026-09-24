# Low-battery email alerts — Design

**Date:** 2026-09-24
**Status:** Implemented (PR #23, ADR-022). The implementation adds a few
review-driven details not in this spec: a negative battery % is treated as
unknown, emails carry Date/Message-ID headers, a non-numeric
`PHOTOFRAME_SMTP_PORT` falls back to 587, and test-email errors read
`Test email failed: <ExceptionType>: <message>`.

## Summary

Have the photo server email the owner when one or more frames report a battery
charge below a configurable threshold (default **20%**). A single email lists
every affected frame. After the first alert, a reminder goes out every 24 hours
while any frame stays low and isn't charging.

This is a server-only change. The firmware already pushes battery state to
`POST /api/status` on every wake.

## Decisions

| Question | Decision |
|----------|----------|
| Transport | **SMTP** via stdlib `smtplib`. No new dependencies |
| SMTP credentials | **`.env`** (`PHOTOFRAME_SMTP_*`), read at startup by `config.py` |
| Recipient + threshold | **Web UI**, stored in the existing `settings` table |
| Alert cadence | **Once, then a reminder every 24h** while low and not charging |
| Scope of a check | **All frames**. One combined email lists every low frame |
| Where the check runs | **In `/api/status`**, as a FastAPI background task after the response |

## Out of scope

- HTML email, templated email bodies, and any third-party email API.
- A "frame went silent" alert, i.e. no report for N hours.
- A periodic scheduler, since checks are driven by frame reports.
- Per-frame thresholds or recipients.
- Deleting or retiring frames (see *Known consequences*).
- Web UI authentication (already tracked in `TODO.md` → Security).

---

## Section 1: Alert rules

### Trigger

`/api/status` stores the reporting frame's status as it does today, then adds
`notifier.check_battery_alerts(db)` as a FastAPI `BackgroundTasks` task. The
check runs after the response has been sent, so it never lengthens the frame's
awake time and never makes `/api/status` fail.

### Check algorithm

`check_battery_alerts` does the following while holding a module-level
`asyncio.Lock`. The server runs a single uvicorn process, so an in-process lock
is enough to stop two concurrent reports from sending duplicate emails.

1. **Enabled?** SMTP counts as *configured* when `config.SMTP_HOST` is set and
   the "From" address (`SMTP_FROM`, or `SMTP_USER` if that is empty) is
   non-empty. If SMTP isn't configured, or the `alert_email` setting is empty,
   return without doing anything.
2. **Load** all frames from the `frames` table: last-known battery status plus
   `low_battery_alerted_at`.
3. **Evaluate** with the pure function `evaluate(frames, threshold, now)`, where
   `T` is the threshold. For each frame:
   - **Re-arm** if `battery_connected` is false, **or** `charging` is true,
     **or** `usb_connected` is true, **or** `battery_percent >= T + 5`. Its
     `low_battery_alerted_at` is cleared to `NULL`.
   - **Low** if `battery_connected` is true, `charging` and `usb_connected` are
     both false, and `battery_percent` is not NULL and `< T`.
   - **Unchanged** otherwise. This covers `T <= percent < T + 5` and a NULL
     `battery_percent`: the frame is not low, and its alert state is kept.
     The 5-point margin stops a reading that bounces around the threshold
     from sending repeated alerts.
   - A low frame is **due** if `low_battery_alerted_at` is NULL (a **new** low
     frame) or `now - low_battery_alerted_at >= 24h`.
   - **Send** if at least one low frame is due.
4. **Apply re-arms**: clear `low_battery_alerted_at` for the re-arm set. This is
   done whether or not an email is sent.
5. **Send** (if the decision says so) one email listing **all** low frames, not
   only the due ones. `send_email` runs through `asyncio.to_thread`.
6. **Record**: only on a successful send, set `low_battery_alerted_at = now`
   for **every** frame in the email. All low frames then share one 24h reminder
   cycle, which gives one daily digest rather than staggered per-frame emails.

Timestamps are UTC ISO 8601 with a `+00:00` suffix, matching the project
convention.

### Known consequences

- A frame that stops reporting while low (probably a flat battery) keeps
  appearing in the daily reminder, with its last-seen time. It re-arms once it
  is charged and reports again. There is no way to delete a frame yet, so a
  frame retired while low would keep being reminded. Accepted for now.
- With only one frame, reminders depend on that frame's own reports, which
  arrive every wake interval (hourly by default).
- If alerts are turned off and back on, a stale `low_battery_alerted_at` older
  than 24h makes the next check send straight away. This is intended.

---

## Section 2: Configuration and web UI

### SMTP settings (`.env` → `config.py`)

| Variable | Default | Notes |
|---|---|---|
| `PHOTOFRAME_SMTP_HOST` | *(empty)* | Empty turns email off completely |
| `PHOTOFRAME_SMTP_PORT` | `587` | |
| `PHOTOFRAME_SMTP_TLS` | `starttls` | `starttls` or `ssl` (implicit TLS, usually port 465) |
| `PHOTOFRAME_SMTP_USER` | *(empty)* | Leave empty for an unauthenticated relay (no login) |
| `PHOTOFRAME_SMTP_PASSWORD` | *(empty)* | e.g. a Gmail app password |
| `PHOTOFRAME_SMTP_FROM` | `SMTP_USER` | "From" address. Required if `SMTP_USER` is empty |

Add all six to `.env.example` with comments. An unrecognised `SMTP_TLS` value
falls back to `starttls` and logs a warning at startup.

### Alert settings (`settings` table)

| Key | Default | Validation |
|---|---|---|
| `alert_email` | `""` | Empty turns alerts off. Otherwise one or more comma-separated addresses; each is trimmed, must contain one `@`, and must not contain whitespace, `\r` or `\n`. This also prevents header injection. |
| `battery_alert_threshold` | `20` | A whole number from 1 to 99 |

New `Database` methods:
- `get_alert_settings() -> {"email": str, "threshold": int}`
- `set_alert_settings(email, threshold)`

### Schema migration

Add the nullable column `frames.low_battery_alerted_at TEXT`. Use a
`_migrate_*` method with a `PRAGMA table_info` check, following the same pattern
as `_migrate_frame_wake_columns`, and add the column to the `CREATE TABLE`
statement for new databases.

New `Database` methods:
- `clear_low_battery_alerts(frame_ids)`
- `set_low_battery_alerted(frame_ids, timestamp)`

### Web UI

A new **Battery alerts** card on the dashboard (`index.html`), below the default
wake interval card and using the same `settings-card` styling:

- A recipient text input, a threshold number input (1–99, `%` suffix), and a
  **Save** button that posts to `POST /settings/alerts`. On success it
  redirects to `/?saved=1`; on a validation failure it redirects to
  `/?error=<message>`. This is the existing toast pattern.
- A **Send test email** button, in its own form, that posts to
  `POST /settings/alerts/test`. It sends **synchronously**, via
  `asyncio.to_thread`, to the saved recipient, so SMTP problems show up at
  once. On success it redirects to `/?notice=Test email sent to <recipient>`;
  on failure it redirects to `/?error=Test email failed: <exception message>`.
  If SMTP isn't configured or no recipient is saved, it redirects to `?error=`
  with an explanation.
- A hint line showing the state:
  - *"SMTP not configured. Set PHOTOFRAME_SMTP_HOST (and SMTP_USER or
    SMTP_FROM) in .env."*
  - *"Alerts off: no recipient set."*
  - *"Alerts on: emailing \<recipient\> when a frame drops below \<T\>%."*
- Add a `notice` query parameter to `/` and render it as a normal (non-error)
  toast in `index.html`.

The web UI has no authentication (an existing Security TODO), so anyone on the
LAN can change these settings or send a test email. This is the same exposure
as the existing settings forms, and nothing extra is added here.

---

## Section 3: Email, code layout, errors, testing

### Email format

Plain text (`email.message.EmailMessage`).

- **Subject:** `Photo frame battery low: <name> (<pct>%)` for one frame, or
  `Photo frame battery low: <n> frames` for several.
- **Body:** one line per low frame, sorted by battery % ascending:
  `<name or MAC> — <pct>% (<mV> mV), last seen <YYYY-MM-DD HH:MM> UTC [NEW]`.
  `[NEW]` marks frames whose `low_battery_alerted_at` was NULL.
- **Footer:** the threshold, and "You'll get a reminder every 24 hours until
  these frames are charged."
- **Test email:** subject `Photo frame test email`, with a body confirming that
  SMTP settings work and showing the current threshold.

### Module layout: new `server/notifier.py`

| Unit | Kind | Responsibility |
|---|---|---|
| `SmtpConfig` | dataclass | Built from `config.SMTP_*` |
| `AlertDecision` | dataclass | `rearm_ids`, `low_frames`, `new_ids`, `send: bool` |
| `evaluate(frames, threshold, now)` | pure | All rules from Section 1 |
| `build_alert_message(decision, threshold, cfg, recipients)` | pure | Low-battery `EmailMessage` |
| `build_test_message(threshold, cfg, recipients)` | pure | Test `EmailMessage` |
| `send_email(cfg, msg)` | sync I/O | `SMTP` + `starttls()` or `SMTP_SSL`, 15s timeout, `login()` only if a user is set |
| `check_battery_alerts(db, now=None)` | async | Holds the lock; runs load → evaluate → re-arm → send → record |

Logging uses `logging.getLogger("uvicorn.error")`, as `database.py` does.

Existing files touched: `main.py` (background task, two routes, dashboard
context, `notice` param), `database.py`, `config.py`, `.env.example`,
`templates/index.html`.

### Error handling

- The whole body of `check_battery_alerts` is wrapped: any exception is logged
  and swallowed, because a background task must never crash anything.
- On an SMTP failure (connection, auth, send) the error is logged and **no
  timestamps are recorded**, so the next frame report retries. If SMTP stays
  broken, that means one failed attempt per report, capped by the 15s timeout
  and off the request path.
- Re-arms from step 4 are already saved before the send, so a send failure
  doesn't undo them.
- The test-email route shows the exception message in the error toast.

### Testing (pytest; no real SMTP)

`notifier.send_email` is monkeypatched to a fake that records the messages it's
given, or raises on demand.

- **`evaluate` unit tests:** the boundary at `T` and `T-1`; the re-arm margin
  (`T+4` keeps the state, `T+5` re-arms); charging, USB, and no-battery
  suppression and re-arm; NULL percent; a reminder at 23h59m (not due) versus
  24h (due); several frames with only one due, where the email still lists all
  low frames; a stale (non-reporting) frame included in the reminder.
- **Message unit tests:** the one-frame and several-frame subjects, the MAC
  fallback when a frame is unnamed, the `[NEW]` marker, and sort order.
- **Integration (API + DB):**
  - A low report sends one email; an immediate second low report sends none.
  - Recovery (charging) followed by a new low reading sends a fresh email.
  - A second frame going low while the first is mid-cycle sends one email
    listing both, and both timestamps are updated.
  - A failed send records nothing, and the next report retries.
  - Two concurrent checks send only one email (lock).
  - Nothing is sent when SMTP or the recipient isn't configured.
  - The migration adds the column to an existing database without it.
  - `POST /settings/alerts` validation: bad email, a header-injection attempt,
    threshold 0 and 100, a non-numeric value, and an empty email that turns
    alerts off.
  - `POST /settings/alerts/test` succeeding, failing with SMTP errors, and
    called while not configured.

### Documentation

- `DECISIONS.md`: ADR-022 (email alerts via SMTP, triggered by status
  reports, one digest for all frames).
- `README.md` and the CLAUDE.md server section: the SMTP env vars and the
  dashboard alert settings.
- `.env.example`: the new variables.
- `TODO.md` and `PROGRESS.md`: record the feature.
