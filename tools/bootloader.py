"""Back up or flash the receiver through its original 8BitDo USB bootloader.

Enter the bootloader first: hold the receiver button while plugging it in, or
run `tools/diagnostic_hid.py --bootloader` while JoyBridge firmware is running.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys
from check_image import APP_BASE, validate
from package_image import package

ROOT = Path(__file__).resolve().parents[1]
VID, PID = 0x0483, 0x5760
FLASH_BASE, FLASH_SIZE = 0x08000000, 0x20000
READ_CHUNK = 23
SUPPORTED_LOADER_SHA256 = "8d733de72baf8cc04ce3ada92e3df39be405b55587a749514c0c756eaad38692"
TAG_OFFSET = 0x1C  # loader-owned installation stamp inside the application vector table
SAVED_TAG = ROOT / "private/device-tag.hex"


def update_packet(command: int, payload: bytes = b"") -> bytes:
    body = bytes([command]) + (struct.pack("<H", len(payload)) + payload if payload else b"")
    request = b"\0\x16" + struct.pack("<H", len(body)) + body
    return (bytes([len(request)]) + request).ljust(64, b"\0")


def check_ack(reply: bytes, command: int) -> None:
    expected = bytes.fromhex("09 00 16 04 00 14 01 00 01" if command == 6 else "06 00 16 01 00 14")
    if not reply.startswith(expected):
        raise RuntimeError(f"Bootloader rejected command {command}: {reply.hex()}")


class Bootloader:
    def __init__(self, timeout_ms: int = 5000):
        import hid
        devices = [d for d in hid.enumerate(VID, PID) if d.get("interface_number") in (0, -1, None)]
        if not devices:
            raise SystemExit("No 8BitDo bootloader (0483:5760) found; enter bootloader mode first")
        self.timeout_ms = timeout_ms
        self.handle = hid.device()
        self.handle.open_path(devices[0]["path"])
        self.handle.set_nonblocking(0)
        while self.handle.read(64, 20):  # discard queued input
            pass

    def close(self) -> None:
        self.handle.close()

    def _write(self, wire: bytes) -> None:
        if self.handle.write(b"\0" + wire.ljust(64, b"\0")) != 65:
            raise RuntimeError("HID write failed")

    def _read(self) -> bytes:
        reply = bytes(self.handle.read(64, self.timeout_ms))
        if not reply:
            raise RuntimeError("Bootloader did not respond")
        return reply

    def read(self, address: int, length: int, progress: str | None = None) -> bytes:
        data = bytearray()
        while len(data) < length:
            count = min(READ_CHUNK, length - len(data))
            # Byte 0 = packet length, byte 1 = packet type, then opcode 0x31.
            self._write(bytes((8, 0, 0x31)) + struct.pack("<HI", count + 4, address + len(data)))
            reply = self._read()
            if reply[0] != count + 9 or reply[2] != 0x14:
                raise RuntimeError(f"Unexpected read reply: {reply.hex()}")
            data += reply[5:5 + count]
            if progress and len(data) % 4096 < READ_CHUNK:
                print(f"\r{progress} {len(data) * 100 // length}%", end="", flush=True)
        if progress:
            print(f"\r{progress} done")
        return bytes(data)

    def update(self, command: int, payload: bytes = b"", wait: bool = True) -> None:
        self._write(update_packet(command, payload))
        if wait:
            check_ack(self._read(), command)

    def launch(self) -> None:
        # Finish erases the loader's scratch pages and starts the application. It sends no ACK.
        self.update(2, wait=False)


def installed_bytes(image: bytes, version: int, tag: bytes) -> bytes:
    """Flash contents the loader leaves behind after programming `image`."""
    result = bytearray(image + b"\xff" * ((-len(image)) % 512))
    result[TAG_OFFSET:TAG_OFFSET + 12] = tag
    struct.pack_into("<I", result, 0x34, version)
    return bytes(result)


def firmware_version() -> int:
    header = (ROOT / "firmware/include/diagnostic_hid.h").read_text()
    return int(re.search(r"#define DIAGNOSTIC_VERSION (0x[0-9a-fA-F]+)u", header)[1], 16)


def installation_tag(loader: Bootloader, override: str | None) -> bytes:
    if override:
        tag = bytes.fromhex(override)
    else:
        tag = loader.read(APP_BASE + TAG_OFFSET, 12)
        if b"\xff" * 4 in (tag[0:4], tag[4:8], tag[8:12]):
            if not SAVED_TAG.exists():
                raise SystemExit("Installation stamp is erased on the device; pass --tag from a backup "
                                 "(12 bytes at flash offset 0x341c)")
            tag = bytes.fromhex(SAVED_TAG.read_text())
    if len(tag) != 12:
        raise SystemExit("--tag must be 12 bytes of hex")
    SAVED_TAG.parent.mkdir(exist_ok=True)
    SAVED_TAG.write_text(tag.hex() + "\n")
    return tag


def flash(args: argparse.Namespace) -> None:
    image = args.image.read_bytes()
    validate(image)
    version = args.version if args.version is not None else firmware_version()
    dat, _ = package(image, version)
    loader = Bootloader()
    try:
        if not args.force:
            loader_sha = hashlib.sha256(loader.read(FLASH_BASE, 0x3000, "Checking bootloader")).hexdigest()
            if loader_sha != SUPPORTED_LOADER_SHA256:
                raise SystemExit(f"Unknown bootloader (sha256 {loader_sha}); use --force to flash anyway")
        tag = installation_tag(loader, args.tag)
        loader.update(1, dat[:28])
        payload = dat[28:]
        for offset in range(0, len(payload), 32):
            loader.update(0, payload[offset:offset + 32])
            if offset % 4096 == 0:
                print(f"\rProgramming {offset * 100 // len(payload)}%", end="", flush=True)
        print("\rProgramming done")
        loader.update(6, tag)
        expected = installed_bytes(image, version, tag)
        if loader.read(APP_BASE, len(expected), "Verifying") != expected:
            raise SystemExit("Readback mismatch; the bootloader stays active, so flash again")
        loader.launch()
        print(f"Flashed {args.image} (version 0x{version:x})")
    finally:
        loader.close()


def backup(args: argparse.Namespace) -> None:
    loader = Bootloader()
    try:
        data = loader.read(FLASH_BASE, FLASH_SIZE, "Reading flash")
    finally:
        loader.close()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"Wrote {args.out} (sha256 {hashlib.sha256(data).hexdigest()})")


def launch(_: argparse.Namespace) -> None:
    loader = Bootloader()
    try:
        loader.launch()
    finally:
        loader.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("flash", help="program a firmware image and start it")
    p.add_argument("image", type=Path, nargs="?", default=ROOT / "build/joybridge.bin")
    p.add_argument("--version", type=lambda s: int(s, 0), help="version stored by the loader (default: firmware header)")
    p.add_argument("--tag", help="installation stamp as 24 hex digits, if the device's copy was erased")
    p.add_argument("--force", action="store_true", help="skip the bootloader revision check")
    p.set_defaults(func=flash)
    p = sub.add_parser("backup", help="read the whole 128 KiB flash")
    p.add_argument("out", type=Path, nargs="?", default=ROOT / "private/flash-backup.bin")
    p.set_defaults(func=backup)
    sub.add_parser("launch", help="leave the bootloader and start the installed application").set_defaults(func=launch)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
