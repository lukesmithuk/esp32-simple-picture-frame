import io
import shutil
import sys
from pathlib import Path

import pytest
from httpx import ASGITransport, AsyncClient
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent.parent))

import config
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
    await tdb.add_image("test.jpg")
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
async def test_upload_image():
    test_jpeg = make_test_jpeg()
    async with AsyncClient(transport=transport, base_url="http://test",
                           follow_redirects=False) as client:
        r = await client.post(
            "/api/upload",
            files={"files": ("photo.jpg", test_jpeg, "image/jpeg")},
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
