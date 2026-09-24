"""Shared test setup.

PHOTOFRAME_DATA_DIR is pointed at a throwaway directory *before* any test
module imports config/main. test_api.py's clean_state fixture deletes the DB,
images and thumbs between tests; without this it would delete the
developer's real server/ data.
"""
import asyncio
import os
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

_TEST_DATA_DIR = tempfile.mkdtemp(prefix="photoframe-test-")
os.environ["PHOTOFRAME_DATA_DIR"] = _TEST_DATA_DIR

sys.path.insert(0, str(Path(__file__).parent.parent))


def pytest_sessionfinish(session, exitstatus):
    shutil.rmtree(_TEST_DATA_DIR, ignore_errors=True)


@pytest.fixture(autouse=True)
def _fresh_notifier_lock():
    """Give each test a new alert lock.

    pytest-asyncio uses a fresh event loop per test, and an asyncio.Lock that
    was ever contended stays bound to the loop it first waited on.
    """
    import notifier
    notifier._lock = asyncio.Lock()
