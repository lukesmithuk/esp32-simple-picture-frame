"""Shared test setup.

PHOTOFRAME_DATA_DIR is pointed at a throwaway directory *before* any test
module imports config/main. test_api.py's clean_state fixture deletes the DB,
images and thumbs between tests; without this it would delete the
developer's real server/ data.
"""
import os
import shutil
import sys
import tempfile
from pathlib import Path

_TEST_DATA_DIR = tempfile.mkdtemp(prefix="photoframe-test-")
os.environ["PHOTOFRAME_DATA_DIR"] = _TEST_DATA_DIR

sys.path.insert(0, str(Path(__file__).parent.parent))


def pytest_sessionfinish(session, exitstatus):
    shutil.rmtree(_TEST_DATA_DIR, ignore_errors=True)
