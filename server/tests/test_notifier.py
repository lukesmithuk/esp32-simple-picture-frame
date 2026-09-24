import smtplib
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import notifier
from database import Database
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


def test_negative_percent_is_ignored_and_keeps_state():
    # Firmware sends -1 when the battery read fails; treat like NULL/unknown.
    d = evaluate([frame(pct=-1, alerted=NOW - timedelta(hours=30))], T, NOW)
    assert d.rearm_ids == []
    assert d.low_frames == []
    assert not d.send


def test_negative_percent_new_frame_does_not_send():
    d = evaluate([frame(pct=-1, alerted=None)], T, NOW)
    assert d.low_frames == []
    assert not d.send
    assert d.rearm_ids == []


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


def test_test_message_has_date_and_message_id_headers():
    msg = notifier.build_test_message(T, CFG, ["me@example.com"])
    assert msg["Date"] is not None
    assert msg["Message-ID"] is not None
    assert msg["Message-ID"].endswith("@test>")


def test_alert_message_has_date_and_message_id_headers():
    d = AlertDecision(low_frames=[frame(pct=18, name="Kitchen")], new_ids={1}, send=True)
    msg = notifier.build_alert_message(d, T, CFG, ["me@example.com"])
    assert msg["Date"] is not None
    assert msg["Message-ID"] is not None


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
