import os
from pathlib import Path

BASE_DIR = Path(__file__).parent
DATA_DIR = Path(os.environ.get("PHOTOFRAME_DATA_DIR", BASE_DIR))
IMAGES_DIR = DATA_DIR / "images"
THUMBS_DIR = DATA_DIR / "thumbs"
DB_PATH = DATA_DIR / "photoframe.db"

API_KEY = os.environ.get("PHOTOFRAME_API_KEY", "changeme")
HOST = os.environ.get("PHOTOFRAME_HOST", "0.0.0.0")
PORT = int(os.environ.get("PHOTOFRAME_PORT", "8080"))

MAX_IMAGE_SIZE = 4 * 1024 * 1024  # 4 MB — matches frame's limit
DISPLAY_WIDTH = 800
DISPLAY_HEIGHT = 480

# Create directories on import
DATA_DIR.mkdir(parents=True, exist_ok=True)
IMAGES_DIR.mkdir(exist_ok=True)
THUMBS_DIR.mkdir(exist_ok=True)
