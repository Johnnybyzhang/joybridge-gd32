"""Encode a raw application image into an 8BitDo update package (.dat)."""
from __future__ import annotations
import argparse
import importlib.util
from pathlib import Path
from check_image import APP_BASE, validate

ROOT = Path(__file__).resolve().parents[1]


def encoder():
    spec = importlib.util.spec_from_file_location("cryptdo8", ROOT / "third_party/8cryptdo/cryptdo8.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def package(image: bytes, version: int) -> tuple[bytes, dict]:
    result = validate(image)
    crypt = encoder()
    return crypt.build_section(image, crypt.GD32(), version, APP_BASE), result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", type=Path)
    p.add_argument("--version", type=lambda s: int(s, 0), required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    dat, _ = package(args.image.read_bytes(), args.version)
    args.out.write_bytes(dat)
    print(f"Wrote {args.out} ({len(dat)} bytes)")


if __name__ == "__main__":
    main()
