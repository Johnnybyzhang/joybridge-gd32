import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def pytest_sessionstart(session):
    if not (ROOT / "build/joybridge.bin").exists():
        subprocess.run([sys.executable, str(ROOT / "tools/build.py")], cwd=ROOT, check=True)
