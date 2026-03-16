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
