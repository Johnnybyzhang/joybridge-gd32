"""Package a checked raw image for the existing loader. OFFLINE; never uploads."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
from check_image import APP_BASE, validate

ROOT = Path(__file__).resolve().parents[1]

def encoder():
    path = ROOT / "third_party/8cryptdo/cryptdo8.py"
    spec = importlib.util.spec_from_file_location("redev_cryptdo8", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module

def package(image: bytes, version: int) -> tuple[bytes, dict]:
    if not 0 <= version <= 0xFFFFFFFF:
        raise ValueError("Version must fit uint32")
    result = validate(image)
    crypt = encoder()
    dat = crypt.build_section(image, crypt.GD32(), version, APP_BASE)
    sections = crypt.parse_image(dat)
    decoded = crypt.decrypt_section(dat, sections[0], crypt.GD32())
    assert decoded == image + b"\xff" * (result["padded_bytes"] - len(image))
    return dat, result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", type=Path)
    p.add_argument("--version", type=lambda s: int(s, 0), required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    dat, result = package(args.image.read_bytes(), args.version)
    # Exclusive create avoids silently replacing a reference firmware package.
    with args.out.open("xb") as out:
        out.write(dat)
    print(json.dumps({**result, "package_bytes": len(dat), "output": str(args.out), "uploaded": False}, indent=2))

if __name__ == "__main__":
    main()
