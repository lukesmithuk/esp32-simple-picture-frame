import pytest
import sys
from pathlib import Path

# Add server directory to path so we can import modules
sys.path.insert(0, str(Path(__file__).parent.parent))

from database import Database


@pytest.fixture
async def db(tmp_path):
    d = Database(tmp_path / "test.db")
    await d.init()
    yield d
    await d.close()


@pytest.mark.asyncio
async def test_add_image(db):
    image_id = await db.add_image("photo1.jpg")
    assert image_id is not None
    images = await db.list_images()
    assert len(images) == 1
    assert images[0]["filename"] == "photo1.jpg"


@pytest.mark.asyncio
async def test_delete_image(db):
    image_id = await db.add_image("photo1.jpg")
    await db.delete_image(image_id)
    images = await db.list_images()
    assert len(images) == 0


@pytest.mark.asyncio
async def test_get_next_image_shuffles(db):
    await db.add_image("a.jpg")
    await db.add_image("b.jpg")
    await db.add_image("c.jpg")

    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "testkey")
    shown = set()
    for _ in range(3):
        img = await db.get_next_image(frame_id)
        assert img is not None
        shown.add(img["filename"])
    assert shown == {"a.jpg", "b.jpg", "c.jpg"}


@pytest.mark.asyncio
async def test_get_next_image_resets_after_full_cycle(db):
    await db.add_image("only.jpg")
    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "testkey")

    img1 = await db.get_next_image(frame_id)
    assert img1["filename"] == "only.jpg"

    # Second call — history full, should reset and return again
    img2 = await db.get_next_image(frame_id)
    assert img2["filename"] == "only.jpg"


@pytest.mark.asyncio
async def test_get_next_image_empty_gallery(db):
    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "testkey")
    img = await db.get_next_image(frame_id)
    assert img is None


@pytest.mark.asyncio
async def test_update_frame_status(db):
    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "testkey")
    await db.update_frame_status(frame_id, {
        "battery_percent": 85,
        "battery_mv": 4050,
        "charging": True,
        "usb_connected": True,
        "sd_free_kb": 12000,
        "firmware_version": "1.0.0",
    })
    frame = await db.get_frame(frame_id)
    assert frame["battery_percent"] == 85
    assert frame["firmware_version"] == "1.0.0"


@pytest.mark.asyncio
async def test_append_logs(db):
    frame_id = await db.get_or_create_frame("AA:BB:CC:DD:EE:FF", "testkey")
    await db.append_logs(frame_id, "line 1\nline 2\n")
    await db.append_logs(frame_id, "line 3\n")
    logs = await db.get_logs(frame_id)
    assert "line 1" in logs
    assert "line 3" in logs
