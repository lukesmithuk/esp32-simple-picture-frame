import importlib
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))


@pytest.fixture
def fresh_config(monkeypatch):
    """Yield the config module; afterwards undo env changes, then reload it.

    Undo must happen *before* the reload, otherwise config is left pointing
    at whatever the test set (e.g. the real server/ data dir).
    """
    import config
    yield config
    monkeypatch.undo()
    importlib.reload(config)


def test_suite_runs_against_isolated_data_dir():
    # conftest.py points PHOTOFRAME_DATA_DIR at a temp dir so test_api's
    # clean_state fixture can never delete the developer's real DB/images.
    import config
    assert config.DATA_DIR != config.BASE_DIR


def test_data_dir_env_redirects_paths(fresh_config, tmp_path, monkeypatch):
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", str(tmp_path))
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == tmp_path
    assert config.DB_PATH == tmp_path / "photoframe.db"
    assert config.IMAGES_DIR == tmp_path / "images"
    assert config.THUMBS_DIR == tmp_path / "thumbs"


def test_data_dir_defaults_to_base_dir(fresh_config, monkeypatch):
    monkeypatch.delenv("PHOTOFRAME_DATA_DIR", raising=False)
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == config.BASE_DIR
    assert config.DB_PATH == config.BASE_DIR / "photoframe.db"


def test_data_dir_empty_string_falls_back_to_base_dir(fresh_config, monkeypatch):
    # An empty value (e.g. `PHOTOFRAME_DATA_DIR=` in .env) must not resolve to CWD.
    monkeypatch.setenv("PHOTOFRAME_DATA_DIR", "")
    config = importlib.reload(fresh_config)
    assert config.DATA_DIR == config.BASE_DIR
