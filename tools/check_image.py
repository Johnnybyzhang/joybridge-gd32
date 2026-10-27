"""Check that a raw application image is laid out the way the original bootloader expects."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct

APP_BASE = 0x08003400
CODE_LIMIT = 0x1B800
JOURNAL_BASE = 0x0801EC00

def validate(image: bytes) -> dict:
    if not 304 <= len(image) <= CODE_LIMIT:
        raise ValueError("Image must contain the 304-byte vector table and fit below the pairing journal")
    padded = (len(image) + 511) & ~511
    erase_end = APP_BASE + ((padded + 1023) & ~1023)
    if padded > CODE_LIMIT or erase_end > JOURNAL_BASE:
        raise ValueError("Loader padding/erase span would overlap the pairing journal")
    sp, reset = struct.unpack_from("<II", image)
    if sp & 7 or not 0x20000800 <= sp <= 0x20004FE0:
        raise ValueError("Initial MSP must be 8-byte aligned with readable loader POP headroom")
    if not reset & 1 or not APP_BASE + 304 <= (reset & ~1) < APP_BASE + len(image):
        raise ValueError("Reset vector must point to Thumb code inside this application")
    for off in (0x1C, 0x20, 0x24, 0x34):
        if image[off:off + 4] != b"\xff" * 4:
            raise ValueError(f"Loader-owned vector word +0x{off:x} must remain erased")
    for index in range(2, 76):
        if index in (7, 8, 9, 10, 13):
            continue
        handler = struct.unpack_from("<I", image, index * 4)[0]
        if not handler & 1 or not APP_BASE + 304 <= (handler & ~1) < APP_BASE + len(image):
            raise ValueError(f"Interrupt vector {index} must point to this application's Thumb code")
    if image[(reset & ~1) - APP_BASE:(reset & ~1) - APP_BASE + 2] != b"\x72\xb6":
        raise ValueError("Reset entry must mask inherited interrupts before using the stack")
    return {"bytes": len(image), "padded_bytes": padded, "erase_end": hex(erase_end),
            "initial_msp": hex(sp), "reset_vector": hex(reset),
            "sha256": hashlib.sha256(image).hexdigest()}

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.image.read_bytes()), indent=2))

if __name__ == "__main__":
    main()
