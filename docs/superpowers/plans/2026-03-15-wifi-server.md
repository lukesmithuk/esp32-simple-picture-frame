# Photo Frame Server — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a self-hosted photo server (Python/FastAPI) that serves images to the ESP32 picture frame, accepts status/log uploads, and provides a web UI for managing photos.

**Architecture:** FastAPI app with SQLite database, file-based image storage, Jinja2 web UI. Single-file server with separate modules for database, API routes, and templates. Deployed on Raspberry Pi.

**Tech Stack:** Python 3, FastAPI, SQLite (via aiosqlite), Jinja2, uvicorn, Pillow (thumbnails)

---

## File Structure

```
server/
  requirements.txt          # Python dependencies
  config.py                 # Server configuration (paths, API key, port)
  database.py               # SQLite schema, connection, queries
  main.py                   # FastAPI app, API routes, web UI routes
  templates/
    index.html              # Gallery + upload + frame status dashboard
  static/
    style.css               # Minimal styling
  images/                   # Uploaded images stored here
  thumbs/                   # Generated thumbnails
  tests/
    test_database.py        # Database layer tests
    test_api.py             # API endpoint tests
```

---

## Chunk 1: Project Setup + Database

### Task 1: Project scaffold

**Files:**
- Create: `server/requirements.txt`
- Create: `server/config.py`

- [ ] **Step 1: Create requirements.txt**

```
fastapi>=0.104.0
uvicorn[standard]>=0.24.0
aiosqlite>=0.19.0
python-multipart>=0.0.6
jinja2>=3.1.2
pillow>=10.0.0
pytest>=7.0.0
httpx>=0.25.0
pytest-asyncio>=0.23.0
```

- [ ] **Step 2: Create config.py**

```python
import os
from pathlib import Path

BASE_DIR = Path(__file__).parent
IMAGES_DIR = BASE_DIR / "images"
THUMBS_DIR = BASE_DIR / "thumbs"
DB_PATH = BASE_DIR / "photoframe.db"

API_KEY = os.environ.get("PHOTOFRAME_API_KEY", "changeme")
HOST = os.environ.get("PHOTOFRAME_HOST", "0.0.0.0")
PORT = int(os.environ.get("PHOTOFRAME_PORT", "8080"))

MAX_IMAGE_SIZE = 4 * 1024 * 1024  # 4 MB — matches frame's limit

# Create directories on import
IMAGES_DIR.mkdir(exist_ok=True)
THUMBS_DIR.mkdir(exist_ok=True)
```

- [ ] **Step 3: Create virtual environment and install dependencies**

```bash
cd server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

- [ ] **Step 4: Commit**

```bash
git add server/requirements.txt server/config.py
git commit -m "feat(server): project scaffold with config and dependencies"
```

### Task 2: Database layer

**Files:**
- Create: `server/database.py`
- Create: `server/tests/test_database.py`

- [ ] **Step 1: Write database tests**

```python
# server/tests/test_database.py
import pytest
import asyncio
from pathlib import Path
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
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd server && source venv/bin/activate
python -m pytest tests/test_database.py -v
```

Expected: ImportError — `database` module doesn't exist yet.

- [ ] **Step 3: Implement database.py**

```python
# server/database.py
import random
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite


class Database:
    def __init__(self, db_path: Path):
        self.db_path = db_path
        self.db = None

    async def init(self):
        self.db = await aiosqlite.connect(self.db_path)
        self.db.row_factory = aiosqlite.Row
        await self.db.executescript("""
            CREATE TABLE IF NOT EXISTS images (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                filename TEXT NOT NULL UNIQUE,
                uploaded_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS frames (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                mac_address TEXT NOT NULL UNIQUE,
                api_key TEXT NOT NULL,
                name TEXT,
                last_seen TEXT,
                battery_percent INTEGER,
                battery_mv INTEGER,
                charging INTEGER,
                usb_connected INTEGER,
                sd_free_kb INTEGER,
                firmware_version TEXT,
                logs TEXT DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                frame_id INTEGER NOT NULL,
                image_id INTEGER NOT NULL,
                shown_at TEXT NOT NULL,
                FOREIGN KEY (frame_id) REFERENCES frames(id),
                FOREIGN KEY (image_id) REFERENCES images(id)
            );
        """)
        await self.db.commit()

    async def close(self):
        if self.db:
            await self.db.close()

    # -- Images --

    async def add_image(self, filename: str) -> int:
        now = datetime.now(timezone.utc).isoformat()
        cursor = await self.db.execute(
            "INSERT INTO images (filename, uploaded_at) VALUES (?, ?)",
            (filename, now),
        )
        await self.db.commit()
        return cursor.lastrowid

    async def delete_image(self, image_id: int):
        await self.db.execute("DELETE FROM history WHERE image_id = ?", (image_id,))
        await self.db.execute("DELETE FROM images WHERE id = ?", (image_id,))
        await self.db.commit()

    async def list_images(self) -> list[dict]:
        cursor = await self.db.execute(
            "SELECT id, filename, uploaded_at FROM images ORDER BY uploaded_at DESC"
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]

    async def get_image_by_id(self, image_id: int) -> dict | None:
        cursor = await self.db.execute(
            "SELECT id, filename, uploaded_at FROM images WHERE id = ?", (image_id,)
        )
        row = await cursor.fetchone()
        return dict(row) if row else None

    # -- Frames --

    async def get_or_create_frame(self, mac_address: str, api_key: str) -> int:
        cursor = await self.db.execute(
            "SELECT id FROM frames WHERE mac_address = ?", (mac_address,)
        )
        row = await cursor.fetchone()
        if row:
            return row["id"]
        now = datetime.now(timezone.utc).isoformat()
        cursor = await self.db.execute(
            "INSERT INTO frames (mac_address, api_key, last_seen) VALUES (?, ?, ?)",
            (mac_address, api_key, now),
        )
        await self.db.commit()
        return cursor.lastrowid

    async def get_frame(self, frame_id: int) -> dict | None:
        cursor = await self.db.execute(
            "SELECT * FROM frames WHERE id = ?", (frame_id,)
        )
        row = await cursor.fetchone()
        return dict(row) if row else None

    async def update_frame_status(self, frame_id: int, status: dict):
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            """UPDATE frames SET
                last_seen = ?, battery_percent = ?, battery_mv = ?,
                charging = ?, usb_connected = ?, sd_free_kb = ?,
                firmware_version = ?
            WHERE id = ?""",
            (
                now,
                status.get("battery_percent"),
                status.get("battery_mv"),
                status.get("charging"),
                status.get("usb_connected"),
                status.get("sd_free_kb"),
                status.get("firmware_version"),
                frame_id,
            ),
        )
        await self.db.commit()

    # -- Logs --

    async def append_logs(self, frame_id: int, text: str):
        await self.db.execute(
            "UPDATE frames SET logs = logs || ? WHERE id = ?",
            (text, frame_id),
        )
        await self.db.commit()

    async def get_logs(self, frame_id: int) -> str:
        cursor = await self.db.execute(
            "SELECT logs FROM frames WHERE id = ?", (frame_id,)
        )
        row = await cursor.fetchone()
        return row["logs"] if row else ""

    # -- Shuffle --

    async def get_next_image(self, frame_id: int) -> dict | None:
        all_images = await self.list_images()
        if not all_images:
            return None

        all_ids = {img["id"] for img in all_images}

        cursor = await self.db.execute(
            "SELECT image_id FROM history WHERE frame_id = ?", (frame_id,)
        )
        shown_rows = await cursor.fetchall()
        shown_ids = {row["image_id"] for row in shown_rows} & all_ids

        candidates = [img for img in all_images if img["id"] not in shown_ids]

        if not candidates:
            await self.db.execute(
                "DELETE FROM history WHERE frame_id = ?", (frame_id,)
            )
            await self.db.commit()
            candidates = all_images

        chosen = random.choice(candidates)
        now = datetime.now(timezone.utc).isoformat()
        await self.db.execute(
            "INSERT INTO history (frame_id, image_id, shown_at) VALUES (?, ?, ?)",
            (frame_id, chosen["id"], now),
        )
        await self.db.commit()
        return chosen
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd server && source venv/bin/activate
python -m pytest tests/test_database.py -v
```

Expected: All 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add server/database.py server/tests/test_database.py
git commit -m "feat(server): database layer with images, frames, shuffle, logs"
```

---

## Chunk 2: API Endpoints

### Task 3: API key auth dependency

**Files:**
- Create: `server/main.py` (initial)
- Create: `server/tests/test_api.py` (initial)

- [ ] **Step 1: Write auth tests**

```python
# server/tests/test_api.py
import pytest
from httpx import AsyncClient, ASGITransport
from main import app

transport = ASGITransport(app=app)


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
```

- [ ] **Step 2: Run tests — expect failure (no main.py)**

```bash
python -m pytest tests/test_api.py -v
```

- [ ] **Step 3: Create main.py with auth dependency**

```python
# server/main.py
import sys
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.staticfiles import StaticFiles

import config
from database import Database

db = Database(config.DB_PATH)


@asynccontextmanager
async def lifespan(app: FastAPI):
    await db.init()
    yield
    await db.close()


app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory=config.BASE_DIR / "static"), name="static")


async def verify_api_key(x_api_key: str = Header(None)):
    if not x_api_key or x_api_key != config.API_KEY:
        raise HTTPException(status_code=401, detail="Invalid API key")
    return x_api_key


async def get_frame_id(
    x_api_key: str = Depends(verify_api_key),
    x_frame_id: str = Header(None),
):
    mac = x_frame_id or "unknown"
    frame_id = await db.get_or_create_frame(mac, x_api_key)
    return frame_id
```

- [ ] **Step 4: Create empty static/style.css so mount doesn't fail**

```bash
mkdir -p server/static
touch server/static/style.css
```

- [ ] **Step 5: Run tests — expect pass**

```bash
python -m pytest tests/test_api.py -v
```

- [ ] **Step 6: Commit**

```bash
git add server/main.py server/static/style.css server/tests/test_api.py
git commit -m "feat(server): FastAPI app with API key auth dependency"
```

### Task 4: GET /api/next endpoint

**Files:**
- Modify: `server/main.py`
- Modify: `server/tests/test_api.py`

- [ ] **Step 1: Add tests for /api/next**

Append to `server/tests/test_api.py`:

```python
import os
import shutil
import config


@pytest.fixture(autouse=True)
async def clean_db():
    """Reset database and images dir for each test."""
    if config.DB_PATH.exists():
        config.DB_PATH.unlink()
    if config.IMAGES_DIR.exists():
        shutil.rmtree(config.IMAGES_DIR)
    config.IMAGES_DIR.mkdir(exist_ok=True)
    if config.THUMBS_DIR.exists():
        shutil.rmtree(config.THUMBS_DIR)
    config.THUMBS_DIR.mkdir(exist_ok=True)
    yield


HEADERS = {"X-API-Key": config.API_KEY, "X-Frame-ID": "AA:BB:CC:DD:EE:FF"}


@pytest.mark.asyncio
async def test_api_next_empty_gallery():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.get("/api/next", headers=HEADERS)
    assert r.status_code == 204


@pytest.mark.asyncio
async def test_api_next_returns_image():
    # Put a test JPEG in the images dir
    test_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 100  # minimal JPEG-like
    (config.IMAGES_DIR / "test.jpg").write_bytes(test_jpeg)

    # Register in DB
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
```

- [ ] **Step 2: Run tests — expect failure**

```bash
python -m pytest tests/test_api.py::test_api_next_returns_image -v
```

- [ ] **Step 3: Add /api/next route to main.py**

Add to `server/main.py`:

```python
from fastapi.responses import Response


@app.get("/api/next")
async def api_next(frame_id: int = Depends(get_frame_id)):
    image = await db.get_next_image(frame_id)
    if image is None:
        return Response(status_code=204)

    image_path = config.IMAGES_DIR / image["filename"]
    if not image_path.exists():
        await db.delete_image(image["id"])
        return Response(status_code=204)

    data = image_path.read_bytes()
    return Response(
        content=data,
        media_type="image/jpeg",
        headers={
            "X-Image-Name": image["filename"],
            "Content-Length": str(len(data)),
        },
    )
```

- [ ] **Step 4: Run tests — expect pass**

```bash
python -m pytest tests/test_api.py -v
```

- [ ] **Step 5: Commit**

```bash
git add server/main.py server/tests/test_api.py
git commit -m "feat(server): GET /api/next endpoint with shuffle"
```

### Task 5: POST /api/status and /api/logs endpoints

**Files:**
- Modify: `server/main.py`
- Modify: `server/tests/test_api.py`

- [ ] **Step 1: Add tests**

Append to `server/tests/test_api.py`:

```python
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


@pytest.mark.asyncio
async def test_api_post_logs():
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post(
            "/api/logs",
            headers={**HEADERS, "Content-Type": "text/plain"},
            content="I (100) main: booted\nI (200) epd: init\n",
        )
    assert r.status_code == 200
```

- [ ] **Step 2: Implement endpoints in main.py**

Add to `server/main.py`:

```python
from fastapi import Body
from pydantic import BaseModel


class FrameStatus(BaseModel):
    battery_percent: int | None = None
    battery_mv: int | None = None
    charging: bool | None = None
    usb_connected: bool | None = None
    sd_free_kb: int | None = None
    firmware_version: str | None = None


@app.post("/api/status")
async def api_status(status: FrameStatus, frame_id: int = Depends(get_frame_id)):
    await db.update_frame_status(frame_id, status.model_dump())
    return {"ok": True}


@app.post("/api/logs")
async def api_logs(request: Request, frame_id: int = Depends(get_frame_id)):
    body = await request.body()
    text = body.decode("utf-8", errors="replace")
    await db.append_logs(frame_id, text)
    return {"ok": True}
```

- [ ] **Step 3: Run all tests**

```bash
python -m pytest tests/ -v
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add server/main.py server/tests/test_api.py
git commit -m "feat(server): POST /api/status and /api/logs endpoints"
```

---

## Chunk 3: Web UI + Image Upload

### Task 6: Image upload endpoint

**Files:**
- Modify: `server/main.py`
- Modify: `server/tests/test_api.py`

- [ ] **Step 1: Add upload test**

Append to `server/tests/test_api.py`:

```python
@pytest.mark.asyncio
async def test_upload_image():
    test_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 100
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.post(
            "/api/upload",
            files={"files": ("photo.jpg", test_jpeg, "image/jpeg")},
        )
    assert r.status_code == 200
    assert (config.IMAGES_DIR / "photo.jpg").exists()


@pytest.mark.asyncio
async def test_delete_image_api():
    test_jpeg = b"\xff\xd8\xff\xe0" + b"\x00" * 100
    (config.IMAGES_DIR / "del.jpg").write_bytes(test_jpeg)

    from database import Database
    tdb = Database(config.DB_PATH)
    await tdb.init()
    image_id = await tdb.add_image("del.jpg")
    await tdb.close()

    async with AsyncClient(transport=transport, base_url="http://test") as client:
        r = await client.delete(f"/api/images/{image_id}")
    assert r.status_code == 200
    assert not (config.IMAGES_DIR / "del.jpg").exists()
```

- [ ] **Step 2: Implement upload and delete in main.py**

Add to `server/main.py`:

```python
from fastapi import File, UploadFile
from fastapi.responses import RedirectResponse
from PIL import Image
import io


def generate_thumbnail(image_path: Path, thumb_path: Path, size=(200, 200)):
    try:
        with Image.open(image_path) as img:
            img.thumbnail(size)
            img.save(thumb_path, "JPEG")
    except Exception:
        pass  # Thumbnail generation is best-effort


@app.post("/api/upload")
async def upload_images(files: list[UploadFile] = File(...)):
    for file in files:
        data = await file.read()
        if len(data) > config.MAX_IMAGE_SIZE:
            continue  # Skip oversized files

        filename = file.filename or "unnamed.jpg"
        # Avoid overwriting — append number if exists
        dest = config.IMAGES_DIR / filename
        counter = 1
        while dest.exists():
            stem = Path(filename).stem
            suffix = Path(filename).suffix
            dest = config.IMAGES_DIR / f"{stem}_{counter}{suffix}"
            counter += 1

        dest.write_bytes(data)
        await db.add_image(dest.name)
        generate_thumbnail(dest, config.THUMBS_DIR / dest.name)

    return RedirectResponse(url="/", status_code=303)


@app.delete("/api/images/{image_id}")
async def delete_image(image_id: int):
    image = await db.get_image_by_id(image_id)
    if not image:
        raise HTTPException(status_code=404, detail="Image not found")

    image_path = config.IMAGES_DIR / image["filename"]
    thumb_path = config.THUMBS_DIR / image["filename"]
    if image_path.exists():
        image_path.unlink()
    if thumb_path.exists():
        thumb_path.unlink()

    await db.delete_image(image_id)
    return {"ok": True}
```

- [ ] **Step 3: Run tests**

```bash
python -m pytest tests/ -v
```

- [ ] **Step 4: Commit**

```bash
git add server/main.py server/tests/test_api.py
git commit -m "feat(server): image upload and delete endpoints with thumbnails"
```

### Task 7: Web UI template

**Files:**
- Create: `server/templates/index.html`
- Modify: `server/static/style.css`
- Modify: `server/main.py`

- [ ] **Step 1: Create index.html template**

```html
<!-- server/templates/index.html -->
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Photo Frame Server</title>
    <link rel="stylesheet" href="/static/style.css">
</head>
<body>
    <h1>Photo Frame Server</h1>

    <section class="upload">
        <h2>Upload Images</h2>
        <form action="/api/upload" method="post" enctype="multipart/form-data">
            <input type="file" name="files" multiple accept="image/jpeg,image/png">
            <button type="submit">Upload</button>
        </form>
    </section>

    <section class="gallery">
        <h2>Gallery ({{ images | length }} images)</h2>
        <div class="grid">
            {% for img in images %}
            <div class="card">
                <img src="/thumbs/{{ img.filename }}" alt="{{ img.filename }}"
                     onerror="this.src='/static/placeholder.svg'">
                <p>{{ img.filename }}</p>
                <button onclick="deleteImage({{ img.id }}, '{{ img.filename }}')">Delete</button>
            </div>
            {% endfor %}
        </div>
    </section>

    <section class="frames">
        <h2>Frames</h2>
        {% if frames %}
        <table>
            <tr>
                <th>MAC</th><th>Last Seen</th><th>Battery</th>
                <th>Charging</th><th>USB</th><th>Firmware</th>
            </tr>
            {% for f in frames %}
            <tr>
                <td>{{ f.mac_address }}</td>
                <td>{{ f.last_seen or "never" }}</td>
                <td>{% if f.battery_percent is not none %}{{ f.battery_percent }}% ({{ f.battery_mv }}mV){% else %}—{% endif %}</td>
                <td>{{ "Yes" if f.charging else "No" }}</td>
                <td>{{ "Yes" if f.usb_connected else "No" }}</td>
                <td>{{ f.firmware_version or "—" }}</td>
            </tr>
            {% endfor %}
        </table>
        {% else %}
        <p>No frames have connected yet.</p>
        {% endif %}
    </section>

    <script>
        async function deleteImage(id, name) {
            if (!confirm("Delete " + name + "?")) return;
            const r = await fetch("/api/images/" + id, { method: "DELETE" });
            if (r.ok) location.reload();
            else alert("Delete failed");
        }
    </script>
</body>
</html>
```

- [ ] **Step 2: Add basic CSS**

```css
/* server/static/style.css */
body { font-family: system-ui, sans-serif; max-width: 960px; margin: 0 auto; padding: 1rem; }
h1 { border-bottom: 2px solid #333; padding-bottom: 0.5rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 1rem; }
.card { border: 1px solid #ddd; padding: 0.5rem; border-radius: 4px; text-align: center; }
.card img { max-width: 100%; height: 150px; object-fit: cover; }
.card p { font-size: 0.8rem; word-break: break-all; margin: 0.25rem 0; }
.card button { background: #c00; color: white; border: none; padding: 0.25rem 0.5rem; cursor: pointer; border-radius: 3px; }
table { width: 100%; border-collapse: collapse; }
th, td { border: 1px solid #ddd; padding: 0.5rem; text-align: left; }
.upload { margin-bottom: 2rem; }
input[type="file"] { margin-right: 0.5rem; }
button[type="submit"] { background: #069; color: white; border: none; padding: 0.5rem 1rem; cursor: pointer; border-radius: 3px; }
```

- [ ] **Step 3: Add web UI route and thumbnail serving to main.py**

Add to `server/main.py`:

```python
from fastapi.responses import FileResponse
from fastapi.templating import Jinja2Templates

templates = Jinja2Templates(directory=config.BASE_DIR / "templates")


@app.get("/")
async def index(request: Request):
    images = await db.list_images()
    cursor = await db.db.execute("SELECT * FROM frames ORDER BY last_seen DESC")
    frames = [dict(row) for row in await cursor.fetchall()]
    return templates.TemplateResponse("index.html", {
        "request": request,
        "images": images,
        "frames": frames,
    })


@app.get("/thumbs/{filename}")
async def serve_thumb(filename: str):
    path = config.THUMBS_DIR / filename
    if not path.exists():
        # Try to generate from original
        orig = config.IMAGES_DIR / filename
        if orig.exists():
            generate_thumbnail(orig, path)
    if path.exists():
        return FileResponse(path, media_type="image/jpeg")
    raise HTTPException(status_code=404)
```

- [ ] **Step 4: Add run block to main.py**

Add at the bottom of `server/main.py`:

```python
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=config.HOST, port=config.PORT)
```

- [ ] **Step 5: Manual test — run server and verify UI**

```bash
cd server && source venv/bin/activate
PHOTOFRAME_API_KEY=testkey python main.py
```

Open `http://localhost:8080` in a browser. Verify: upload form visible, gallery grid renders, frame status table shows.

- [ ] **Step 6: Commit**

```bash
git add server/templates/ server/static/ server/main.py
git commit -m "feat(server): web UI with gallery, upload, and frame status"
```

### Task 8: Run script

**Files:**
- Create: `server/run.sh`

- [ ] **Step 1: Create run script**

```bash
#!/bin/bash
# server/run.sh — start the photo frame server
cd "$(dirname "$0")"
export PHOTOFRAME_API_KEY="${PHOTOFRAME_API_KEY:-changeme}"
export PHOTOFRAME_PORT="${PHOTOFRAME_PORT:-8080}"
source venv/bin/activate 2>/dev/null || { echo "Run: python3 -m venv venv && pip install -r requirements.txt"; exit 1; }
exec python main.py
```

```bash
chmod +x server/run.sh
```

- [ ] **Step 2: Commit**

```bash
git add server/run.sh
git commit -m "feat(server): run script for easy startup"
```
