"""Low-battery email alerts.

On every frame status report, check_battery_alerts() looks at *all* frames
and sends one combined email when any low frame is due an alert (see
docs/superpowers/specs/2026-09-24-battery-alert-email-design.md).
"""
import asyncio
import logging
import smtplib
import ssl
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage

import config

logger = logging.getLogger("uvicorn.error")

REARM_MARGIN = 5  # percentage points above the threshold before re-arming
REMINDER_INTERVAL = timedelta(hours=24)
SMTP_TIMEOUT = 15  # seconds

# Serialises check → send → record so two simultaneous frame reports can't
# both send the same alert. The server is a single uvicorn process.
_lock = asyncio.Lock()


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
