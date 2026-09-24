import io
import shutil
import smtplib
import sys
from pathlib import Path
from urllib.parse import unquote

import pytest
from httpx import ASGITransport, AsyncClient
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent.parent))

import config
import notifier
from main import app, db

transport = ASGITransport(app=app)

HEADERS = {"X-API-Key": config.API_KEY, "X-Frame-ID": "AA:BB:CC:DD:EE:FF"}


def make_test_jpeg() -> bytes:
    """Create a minimal valid JPEG in memory."""
    img = Image.new("RGB", (10, 10), color=(255, 0, 0))
    buf = io.BytesIO()
    img.save(buf, "JPEG")
    return buf.getvalue()


@pytest.fixture(autouse=True)
async def clean_state():
    """Reset database and image dirs for each test."""
    if db.db:
        await db.close()
    if config.DB_PATH.exists():
        config.DB_PATH.unlink()
    if config.IMAGES_DIR.exists():
        shutil.rmtree(config.IMAGES_DIR)
    config.IMAGES_DIR.mkdir(exist_ok=True)
    if config.THUMBS_DIR.exists():
        shutil.rmtree(config.THUMBS_DIR)
    config.THUMBS_DIR.mkdir(exist_ok=True)
    await db.init()
    yield
    await db.close()


# ── Auth ─────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_api_next_no_auth():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/api/next")
    assert r.status_code == 401


@pytest.mark.asyncio
async def test_api_next_bad_key():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/api/next", headers={"X-API-Key": "wrong"})
    assert r.status_code == 401


# ── GET /api/next ────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_api_next_empty_gallery():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/api/next", headers=HEADERS)
    assert r.status_code == 204


@pytest.mark.asyncio
async def test_api_next_returns_image():
    test_jpeg = make_test_jpeg()
    (config.IMAGES_DIR / "test.jpg").write_bytes(test_jpeg)

    from database import Database
    tdb = Database(config.DB_PATH)
    await tdb.init()
    image_id = await tdb.add_image("test.jpg")
    # Create frame and assign the image to it.
    frame_id = await tdb.get_or_create_frame("AA:BB:CC:DD:EE:FF", config.API_KEY)
    await tdb.assign_image_to_frame(image_id, frame_id)
    await tdb.close()

    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/api/next", headers=HEADERS)
    assert r.status_code == 200
    assert r.headers["content-type"] == "image/jpeg"
    assert r.headers["x-image-name"] == "test.jpg"
    assert len(r.content) == len(test_jpeg)


# ── POST /api/status ────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_api_post_status():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post("/api/status", headers=HEADERS, json={
            "battery_percent": 85,
            "battery_mv": 4050,
            "charging": True,
            "usb_connected": True,
            "sd_free_kb": 12000,
            "firmware_version": "1.0.0",
        })
    assert r.status_code == 200


# ── POST /api/logs ──────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_api_post_logs():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post(
            "/api/logs",
            headers={**HEADERS, "Content-Type": "text/plain"},
            content="I (100) main: booted\nI (200) epd: init\n",
        )
    assert r.status_code == 200


# ── Upload + Delete ─────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_upload_no_auth():
    test_jpeg = make_test_jpeg()
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post(
            "/api/upload",
            files={"files": ("photo.jpg", test_jpeg, "image/jpeg")},
        )
    assert r.status_code == 401


@pytest.mark.asyncio
async def test_upload_image():
    test_jpeg = make_test_jpeg()
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post(
            "/api/upload",
            files={"files": ("photo.jpg", test_jpeg, "image/jpeg")},
            data={"api_key": config.API_KEY},
        )
    assert r.status_code == 303
    assert (config.IMAGES_DIR / "photo.jpg").exists()
    images = await db.list_images()
    assert any(img["filename"] == "photo.jpg" for img in images)


@pytest.mark.asyncio
async def test_delete_image_api():
    test_jpeg = make_test_jpeg()
    (config.IMAGES_DIR / "del.jpg").write_bytes(test_jpeg)

    from database import Database
    tdb = Database(config.DB_PATH)
    await tdb.init()
    image_id = await tdb.add_image("del.jpg")
    await tdb.close()

    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.delete(f"/api/images/{image_id}",
                                headers={"X-API-Key": config.API_KEY})
    assert r.status_code == 200
    assert not (config.IMAGES_DIR / "del.jpg").exists()


# ── Settings ───────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_zero_wake_interval_rejected():
    """Global wake interval of 0h 0m 0s is rejected via a redirect carrying an error toast."""
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings", data={
            "wake_hours": "0",
            "wake_minutes": "0",
            "wake_seconds": "0",
        })
    assert r.status_code == 303
    assert "error=" in r.headers["location"]


# ── Low-battery alerts ──────────────────────────────────────────────────────

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
    assert "SMTPAuthenticationError" in location
    assert "bad credentials" in location


@pytest.mark.asyncio
async def test_test_email_requires_smtp(monkeypatch):
    monkeypatch.setattr(config, "SMTP_HOST", "")
    await db.set_alert_settings("me@example.com", 20)
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post("/settings/alerts/test")
    location = unquote(r.headers["location"])
    assert "SMTP not configured" in location
    assert ("SMTP not configured. Set PHOTOFRAME_SMTP_HOST "
            "(and SMTP_USER or SMTP_FROM) in .env.") in location


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


# ── Health ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_healthz_no_auth():
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        resp = await ac.get("/healthz")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}
