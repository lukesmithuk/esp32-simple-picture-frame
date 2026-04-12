import io
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import (
    Body, Depends, FastAPI, File, Form, Header, HTTPException,
    Request, UploadFile,
)
from fastapi.responses import FileResponse, RedirectResponse, Response
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from PIL import Image
from pydantic import BaseModel

import config
from database import Database

db = Database(config.DB_PATH)


@asynccontextmanager
async def lifespan(app: FastAPI):
    await db.init()
    await db.sync_images(config.IMAGES_DIR)
    yield
    await db.close()


app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory=config.BASE_DIR / "static"), name="static")
templates = Jinja2Templates(directory=config.BASE_DIR / "templates")

_css_path = config.BASE_DIR / "static" / "style.css"
_css_version = str(int(_css_path.stat().st_mtime)) if _css_path.exists() else "0"


async def _nav_context() -> dict:
    """Common context for all templates (nav bar frames list, css version)."""
    frames = await db.list_frames()
    return {"nav_frames": frames, "css_v": _css_version}


# ── Auth ─────────────────────────────────────────────────────────────────

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


# ── API: Image fetch ─────────────────────────────────────────────────────

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

    # Per-frame wake interval, falling back to global.
    wake = await db.get_frame_wake_interval(frame_id)
    if wake is None:
        wake = await db.get_wake_interval()

    return Response(
        content=data,
        media_type="image/jpeg",
        headers={
            "X-Image-Name": image["filename"],
            "Content-Length": str(len(data)),
            "X-Wake-Hours": str(wake["hours"]),
            "X-Wake-Minutes": str(wake["minutes"]),
            "X-Wake-Seconds": str(wake["seconds"]),
        },
    )


# ── API: Status + Logs ──────────────────────────────────────────────────

class FrameStatus(BaseModel):
    battery_connected: bool | None = None
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


# ── API: Upload + Delete ────────────────────────────────────────────────

def generate_thumbnail(image_path: Path, thumb_path: Path, size=(200, 200)):
    try:
        with Image.open(image_path) as img:
            img.thumbnail(size)
            img.save(thumb_path, "JPEG")
    except Exception:
        pass  # Thumbnail generation is best-effort


@app.post("/api/upload")
async def upload_images(
    files: list[UploadFile] = File(...),
    api_key: str = Form(None),
    x_api_key: str = Header(None),
):
    # Accept API key from form field (web UI) or header (API client)
    key = api_key or x_api_key
    if not key or key != config.API_KEY:
        raise HTTPException(status_code=401, detail="Invalid API key")
    uploaded = 0
    for file in files:
        data = await file.read()
        if len(data) == 0 or not file.filename:
            continue  # Skip empty / no file selected
        if len(data) > config.MAX_IMAGE_SIZE:
            continue  # Skip oversized files

        # Convert to baseline JPEG, resize to fit display
        try:
            img = Image.open(io.BytesIO(data))
            img = img.convert("RGB")
        except Exception:
            continue  # Skip unreadable images

        # Resize if larger than display (cover mode: fit largest dimension)
        max_w, max_h = config.DISPLAY_WIDTH, config.DISPLAY_HEIGHT
        if img.width > max_w or img.height > max_h:
            # Scale so the smaller dimension matches the display
            scale = max(max_w / img.width, max_h / img.height)
            new_w = int(img.width * scale)
            new_h = int(img.height * scale)
            img = img.resize((new_w, new_h), Image.LANCZOS)

        # Force .jpg extension
        stem = Path(file.filename).stem
        filename = f"{stem}.jpg"
        dest = config.IMAGES_DIR / filename
        counter = 1
        while dest.exists():
            dest = config.IMAGES_DIR / f"{stem}_{counter}.jpg"
            counter += 1

        img.save(dest, "JPEG", quality=95, progressive=False)
        image_id = await db.add_image(dest.name)
        generate_thumbnail(dest, config.THUMBS_DIR / dest.name)
        # Assign to all existing frames by default.
        await db.assign_image_to_all_frames(image_id)
        uploaded += 1

    return RedirectResponse(url=f"/gallery?uploaded={uploaded}", status_code=303)


@app.delete("/api/images/{image_id}")
async def delete_image(image_id: int, _: str = Depends(verify_api_key)):
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


# ── Web UI ───────────────────────────────────────────────────────────────

@app.get("/thumbs/{filename}")
async def serve_thumb(filename: str):
    path = config.THUMBS_DIR / filename
    if not path.resolve().is_relative_to(config.THUMBS_DIR.resolve()):
        raise HTTPException(status_code=400, detail="Invalid filename")
    if not path.exists():
        orig = config.IMAGES_DIR / filename
        if not orig.resolve().is_relative_to(config.IMAGES_DIR.resolve()):
            raise HTTPException(status_code=400, detail="Invalid filename")
        if orig.exists():
            generate_thumbnail(orig, path)
    if path.exists():
        return FileResponse(path, media_type="image/jpeg")
    raise HTTPException(status_code=404)


# ── Web UI: Dashboard ──────────────────────────────────────────────────

@app.get("/")
async def index(request: Request, saved: int | None = None, error: str | None = None):
    frames = await db.list_frames()
    # Add image count per frame (single query).
    counts = await db.get_frame_image_counts()
    for frame in frames:
        frame["image_count"] = counts.get(frame["id"], 0)
    wake = await db.get_wake_interval()
    nav = await _nav_context()
    return templates.TemplateResponse(request, "index.html", context={
        **nav,
        "frames": frames,
        "wake": wake,
        "saved": saved,
        "error": error,
    })


# ── Web UI: Shared Gallery ──────────────────────────────────────────────

@app.get("/gallery")
async def gallery(request: Request, uploaded: int | None = None):
    images = await db.list_images()
    frames = await db.list_frames()
    # Build assignment map: image_id → [frame_ids] (single query).
    assignments = await db.get_all_image_assignments()
    nav = await _nav_context()
    return templates.TemplateResponse(request, "gallery.html", context={
        **nav,
        "images": images,
        "frames": frames,
        "assignments": assignments,
        "uploaded": uploaded,
        "api_key": config.API_KEY,
    })


# ── Web UI: Frame Detail Page ──────────────────────────────────────────

@app.get("/frames/{frame_id}")
async def frame_detail(request: Request, frame_id: int, saved: int | None = None, error: str | None = None):
    frame = await db.get_frame(frame_id)
    if not frame:
        raise HTTPException(status_code=404, detail="Frame not found")
    images = await db.get_frame_images(frame_id)
    logs = await db.get_logs(frame_id)
    frame_wake = await db.get_frame_wake_interval(frame_id)
    global_wake = await db.get_wake_interval()
    nav = await _nav_context()
    return templates.TemplateResponse(request, "frame.html", context={
        **nav,
        "frame": frame,
        "images": images,
        "logs": logs,
        "frame_wake": frame_wake,
        "global_wake": global_wake,
        "saved": saved,
        "error": error,
    })


@app.post("/frames/{frame_id}/settings")
async def save_frame_settings(frame_id: int, request: Request):
    frame = await db.get_frame(frame_id)
    if not frame:
        raise HTTPException(status_code=404, detail="Frame not found")

    form = await request.form()

    # Update name.
    name = form.get("name", "").strip()
    await db.update_frame_name(frame_id, name or None)

    # Update per-frame wake interval.
    use_custom = form.get("use_custom_wake") == "on"
    if use_custom:
        result = _validate_wake_interval(form)
        if isinstance(result, str):
            from urllib.parse import quote
            return RedirectResponse(
                url=f"/frames/{frame_id}?error={quote(result)}", status_code=303)
        hours, minutes, seconds = result
        await db.update_frame_wake_interval(frame_id, hours, minutes, seconds)
    else:
        await db.update_frame_wake_interval(frame_id, None, None, None)

    return RedirectResponse(url=f"/frames/{frame_id}?saved=1", status_code=303)


# ── Web UI: Image assignment ───────────────────────────────────────────

@app.post("/api/images/{image_id}/assign")
async def assign_image(image_id: int, request: Request, _: str = Depends(verify_api_key)):
    """Set which frames an image is assigned to."""
    body = await request.json()
    frame_ids = body.get("frame_ids", [])

    image = await db.get_image_by_id(image_id)
    if not image:
        raise HTTPException(status_code=404, detail="Image not found")

    await db.set_image_assignments(image_id, frame_ids)
    return {"ok": True}


# ── Web UI: Global Settings ────────────────────────────────────────────

def _validate_wake_interval(form) -> tuple[int, int, int] | str:
    """Validate wake interval from form data. Returns (h, m, s) or error string."""
    try:
        hours = int(form.get("wake_hours", 1))
        minutes = int(form.get("wake_minutes", 0))
        seconds = int(form.get("wake_seconds", 0))
    except (ValueError, TypeError):
        return "Invalid wake interval values"
    if not (0 <= hours <= 720):
        return "Hours must be 0–720"
    if not (0 <= minutes <= 59):
        return "Minutes must be 0–59"
    if not (0 <= seconds <= 59):
        return "Seconds must be 0–59"
    if hours == 0 and minutes == 0 and seconds == 0:
        return "Wake interval cannot be zero"
    return (hours, minutes, seconds)


@app.post("/settings")
async def save_settings(request: Request):
    form = await request.form()
    result = _validate_wake_interval(form)
    if isinstance(result, str):
        from urllib.parse import quote
        return RedirectResponse(url=f"/?error={quote(result)}", status_code=303)
    hours, minutes, seconds = result
    await db.set_wake_interval(hours, minutes, seconds)
    return RedirectResponse(url="/?saved=1", status_code=303)


# ── Web UI: Logs ────────────────────────────────────────────────────────

@app.get("/logs/{frame_id}")
async def frame_logs(request: Request, frame_id: int):
    frame = await db.get_frame(frame_id)
    if not frame:
        raise HTTPException(status_code=404, detail="Frame not found")
    logs = await db.get_logs(frame_id)
    nav = await _nav_context()
    return templates.TemplateResponse(request, "logs.html", context={
        **nav,
        "frame": frame,
        "logs": logs,
    })


# ── Entry point ──────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=config.HOST, port=config.PORT)
