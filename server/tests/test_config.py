import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))


def test_data_dir_env_redirects_paths(tmp_path, monkeypatch):
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", str(tmp_path))
    import config
    try:
        importlib.reload(config)
        assert config.DATA_DIR == tmp_path
        assert config.DB_PATH == tmp_path / "photoframe.db"
        assert config.IMAGES_DIR == tmp_path / "images"
        assert config.THUMBS_DIR == tmp_path / "thumbs"
    finally:
        monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
        importlib.reload(config)  # restore module-level paths for other tests


def test_data_dir_defaults_to_base_dir(monkeypatch):
    monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
    import config
    importlib.reload(config)
    assert config.DATA_DIR == config.BASE_DIR
    assert config.DB_PATH == config.BASE_DIR / "photoframe.db"
