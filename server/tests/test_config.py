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


SMTP_VARS = (
    "PHOTOFRAME_SMTP_HOST", "PHOTOFRAME_SMTP_PORT", "PHOTOFRAME_SMTP_TLS",
    "PHOTOFRAME_SMTP_USER", "PHOTOFRAME_SMTP_PASSWORD", "PHOTOFRAME_SMTP_FROM",
)


def _clear_smtp_env(monkeypatch):
    for var in SMTP_VARS:
        monkeypatch.delenv(var, raising=False)


def test_smtp_defaults(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    config = importlib.reload(fresh_config)
    assert config.SMTP_HOST == ""
    assert config.SMTP_PORT == 587
    assert config.SMTP_TLS == "starttls"
    assert config.SMTP_USER == ""
    assert config.SMTP_PASSWORD == ""
    assert config.SMTP_FROM == ""


def test_smtp_from_env(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_HOST", "smtp.example.com")
    monkeypatch.setenv("PHOTOFRAME_SMTP_PORT", "465")
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "SSL")
    monkeypatch.setenv("PHOTOFRAME_SMTP_USER", "user@example.com")
    monkeypatch.setenv("PHOTOFRAME_SMTP_PASSWORD", "secret")
    monkeypatch.setenv("PHOTOFRAME_SMTP_FROM", "frames@example.com")
    config = importlib.reload(fresh_config)
    assert config.SMTP_HOST == "smtp.example.com"
    assert config.SMTP_PORT == 465
    assert config.SMTP_TLS == "ssl"
    assert config.SMTP_USER == "user@example.com"
    assert config.SMTP_PASSWORD == "secret"
    assert config.SMTP_FROM == "frames@example.com"


def test_smtp_from_falls_back_to_user(fresh_config, monkeypatch):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_USER", "user@example.com")
    config = importlib.reload(fresh_config)
    assert config.SMTP_FROM == "user@example.com"


def test_smtp_blank_values_use_defaults(fresh_config, monkeypatch):
    # `PHOTOFRAME_SMTP_PORT=` (blank line in .env) must not crash int().
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_PORT", "")
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "")
    config = importlib.reload(fresh_config)
    assert config.SMTP_PORT == 587
    assert config.SMTP_TLS == "starttls"


def test_smtp_invalid_tls_falls_back_to_starttls(fresh_config, monkeypatch, caplog):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_TLS", "bogus")
    config = importlib.reload(fresh_config)
    assert config.SMTP_TLS == "starttls"
    assert "PHOTOFRAME_SMTP_TLS" in caplog.text


def test_smtp_invalid_port_falls_back_to_587(fresh_config, monkeypatch, caplog):
    _clear_smtp_env(monkeypatch)
    monkeypatch.setenv("PHOTOFRAME_SMTP_PORT", "not-a-number")
    config = importlib.reload(fresh_config)
    assert config.SMTP_PORT == 587
    assert "PHOTOFRAME_SMTP_PORT" in caplog.text
