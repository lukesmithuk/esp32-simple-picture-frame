import logging
import os
from pathlib import Path

BASE_DIR = Path(__file__).parent
DATA_DIR = Path(os.environ.get("PHOTOFRAME_DATA_DIR") or BASE_DIR)
IMAGES_DIR = DATA_DIR / "images"
THUMBS_DIR = DATA_DIR / "thumbs"
DB_PATH = DATA_DIR / "photoframe.db"

API_KEY = os.environ.get("PHOTOFRAME_API_KEY", "changeme")
HOST = os.environ.get("PHOTOFRAME_HOST", "0.0.0.0")
PORT = int(os.environ.get("PHOTOFRAME_PORT", "8080"))

# Email alerts (SMTP). An empty host disables email entirely.
SMTP_HOST = os.environ.get("PHOTOFRAME_SMTP_HOST", "")
SMTP_PORT = int(os.environ.get("PHOTOFRAME_SMTP_PORT") or "587")
SMTP_TLS = (os.environ.get("PHOTOFRAME_SMTP_TLS") or "starttls").strip().lower()
if SMTP_TLS not in ("starttls", "ssl"):
    logging.getLogger("uvicorn.error").warning(
        "PHOTOFRAME_SMTP_TLS=%r not recognised (use starttls or ssl); using starttls",
        SMTP_TLS,
    )
    SMTP_TLS = "starttls"
SMTP_USER = os.environ.get("PHOTOFRAME_SMTP_USER", "")
SMTP_PASSWORD = os.environ.get("PHOTOFRAME_SMTP_PASSWORD", "")
SMTP_FROM = os.environ.get("PHOTOFRAME_SMTP_FROM") or SMTP_USER

MAX_IMAGE_SIZE = 4 * 1024 * 1024  # 4 MB — matches frame's limit
DISPLAY_WIDTH = 800
DISPLAY_HEIGHT = 480

# Create directories on import
DATA_DIR.mkdir(parents=True, exist_ok=True)
IMAGES_DIR.mkdir(exist_ok=True)
THUMBS_DIR.mkdir(exist_ok=True)
