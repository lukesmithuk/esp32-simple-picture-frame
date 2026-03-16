import io
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import (
    Body, Depends, FastAPI, File, Header, HTTPException,
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
    yield
    await db.close()


app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory=config.BASE_DIR / "static"), name="static")
templates = Jinja2Templates(directory=config.BASE_DIR / "templates")


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
    return Response(
        content=data,
        media_type="image/jpeg",
        headers={
            "X-Image-Name": image["filename"],
            "Content-Length": str(len(data)),
        },
    )


# ── API: Status + Logs ──────────────────────────────────────────────────

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


# ── API: Upload + Delete ────────────────────────────────────────────────

def generate_thumbnail(image_path: Path, thumb_path: Path, size=(200, 200)):
    try:
        with Image.open(image_path) as img:
            img.thumbnail(size)
            img.save(thumb_path, "JPEG")
    except Exception:
        pass  # Thumbnail generation is best-effort


@app.post("/api/upload")
async def upload_images(files: list[UploadFile] = File(...)):
    uploaded = 0
    for file in files:
        data = await file.read()
        if len(data) == 0 or not file.filename:
            continue  # Skip empty / no file selected
        if len(data) > config.MAX_IMAGE_SIZE:
            continue  # Skip oversized files

        # Convert to baseline JPEG (handles progressive JPEG, PNG, etc.)
        try:
            img = Image.open(io.BytesIO(data))
            img = img.convert("RGB")  # Strip alpha, ensure RGB
        except Exception:
            continue  # Skip unreadable images

        # Force .jpg extension
        stem = Path(file.filename).stem
        filename = f"{stem}.jpg"
        dest = config.IMAGES_DIR / filename
        counter = 1
        while dest.exists():
            dest = config.IMAGES_DIR / f"{stem}_{counter}.jpg"
            counter += 1

        img.save(dest, "JPEG", quality=95, progressive=False)
        await db.add_image(dest.name)
        generate_thumbnail(dest, config.THUMBS_DIR / dest.name)
        uploaded += 1

    return RedirectResponse(url=f"/?uploaded={uploaded}", status_code=303)


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


# ── Web UI ───────────────────────────────────────────────────────────────

@app.get("/thumbs/{filename}")
async def serve_thumb(filename: str):
    path = config.THUMBS_DIR / filename
    if not path.exists():
        orig = config.IMAGES_DIR / filename
        if orig.exists():
            generate_thumbnail(orig, path)
    if path.exists():
        return FileResponse(path, media_type="image/jpeg")
    raise HTTPException(status_code=404)


@app.get("/")
async def index(request: Request, uploaded: int | None = None):
    images = await db.list_images()
    frames = await db.list_frames()
    return templates.TemplateResponse("index.html", {
        "request": request,
        "images": images,
        "frames": frames,
        "uploaded": uploaded,
    })


@app.get("/logs/{frame_id}")
async def frame_logs(request: Request, frame_id: int):
    frame = await db.get_frame(frame_id)
    if not frame:
        raise HTTPException(status_code=404, detail="Frame not found")
    logs = await db.get_logs(frame_id)
    return templates.TemplateResponse("logs.html", {
        "request": request,
        "frame": frame,
        "logs": logs,
    })


# ── Entry point ──────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=config.HOST, port=config.PORT)
