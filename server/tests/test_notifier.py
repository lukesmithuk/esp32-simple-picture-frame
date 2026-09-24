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
