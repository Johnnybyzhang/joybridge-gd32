"""Bounded application update through the ORIGINAL 8BitDo HID bootloader.

Defaults to offline validation. --execute is a real device write operation.
Uses the established backup/stamp and the exact new image and build manifest.
Never implements raw flash writes, metadata writes or bootloader replacement.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from check_image import APP_BASE, validate
from package_image import package

ROOT = Path(__file__).resolve().parents[1]
from usb_readonly_backup import ReadSession, device_description, enumerate_devices

def sha(data):
    return hashlib.sha256(data).hexdigest()

def packet(command: int, payload: bytes = b"") -> bytes:
    sizes = {0: 32, 1: 28, 2: 0, 6: 12}
    if command not in sizes or len(payload) != sizes[command]:
        raise ValueError("Only fixed ordinary application-update packets are supported")
    body = bytes([command])
    if payload:
        body += struct.pack("<H", len(payload)) + payload
    request = b"\0\x16" + struct.pack("<H", len(body)) + body
    wire = bytes([len(request)]) + request
    return wire.ljust(64, b"\0")

def ack(reply: bytes, tag=False):
    prefix = bytes.fromhex("09 00 16 04 00 14 01 00 01" if tag else "06 00 16 01 00 14")
    if len(reply) != 64 or not reply.startswith(prefix):
        raise RuntimeError(f"Unexpected update ACK/abort: {reply.hex()}")

def installed_bytes(image: bytes, version: int, tag: bytes) -> bytes:
    result = bytearray(image + b"\xff" * ((-len(image)) % 512))
    result[0x1c:0x28] = tag
    struct.pack_into("<I", result, 0x34, version)
    return bytes(result)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, default=ROOT / "build/hid-diagnostic.bin")
    p.add_argument("--pre-backup", type=Path, required=True)
    p.add_argument("--out", type=Path)
    p.add_argument("--execute", action="store_true")
    p.add_argument("--launch-only", action="store_true", help="Run normal bootloader finish to launch the already-installed image")
    p.add_argument("--previous-install", type=Path, help="Prior successful transaction's expected-installed.bin; reuses the known installation")
    args = p.parse_args()
    image = args.image.read_bytes()
    checked = validate(image)
    manifest = json.loads(args.image.with_suffix(".json").read_text())
    if manifest.get("kind") != "hid-diagnostic" or manifest.get("sha256") != sha(image):
        raise ValueError("Only the exact build/hid-diagnostic and its matching manifest may be installed")
    status = json.loads((args.pre_backup / "status.json").read_text())
    original = (args.pre_backup / "flash_backup.bin").read_bytes()
    if status.get("state") != "complete" or not status["dump"]["two_full_reads_match"] or len(original) != 0x20000 or status["dump"]["sha256"] != sha(original):
        raise ValueError("Use the established matching backup")
    # Identify the supported loader without distributing its bytes.
    if sha(original[:0x3000]) != "8d733de72baf8cc04ce3ada92e3df39be405b55587a749514c0c756eaad38692":
        raise ValueError("Unsupported bootloader revision; nothing written")
    tag = original[0x341c:0x3428]
    if any(tag[i:i+4] == b"\xff"*4 for i in (0,4,8)):
        raise ValueError("Backup lacks the existing device installation stamp")
    dat, _ = package(image, manifest["version"])
    expected = installed_bytes(image, manifest["version"], tag)
    previous = args.previous_install.read_bytes() if args.previous_install else original[0x3400:]
    summary = {"image_sha256": sha(image), "package_sha256": sha(dat),
               "installed_sha256": sha(expected), "base": APP_BASE, "bytes": len(expected),
               "erase_end_exclusive": checked["erase_end_exclusive"], "version": manifest["version"],
               "pre_backup_sha256": sha(original), "launch_only": args.launch_only,
               "executed": False}
    if not args.execute:
        print(json.dumps(summary, indent=2)); return
    if args.out is None:
        raise ValueError("--execute requires a new --out directory for the transaction log")
    import hid
    devices = enumerate_devices(hid)
    if len(devices) != 1:
        raise RuntimeError("Exactly one original bootloader HID interface must be present")
    description = device_description(devices[0], 0)
    if not description["eligible"] or description["path_sha256"] != status["device"]["path_sha256"]:
        raise RuntimeError("Device/interface/path differs from the fresh backup")
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / "package.dat").write_bytes(dat)
    (args.out / "expected-installed.bin").write_bytes(expected)
    handle = hid.device(); handle.open_path(devices[0]["path"]); handle.set_nonblocking(0)
    session = ReadSession(handle, args.out / "transaction.jsonl", 5000)
    def read_region(address, length, label):
        data = bytearray()
        while len(data) < length:
            data.extend(session.read_chunk(address + len(data), min(23, length - len(data)), label))
        return bytes(data)
    def send(command, payload=b"", finish=False):
        wire = packet(command, payload)
        session.log("update_request", command=command, wire_hex=wire.hex())
        if handle.write(b"\0" + wire) != 65:
            raise RuntimeError("Incomplete HID write; update stopped")
        if not finish:
            response = bytes(handle.read(64, 5000))
            session.log("update_response", command=command, wire_hex=response.hex())
            ack(response, tag=command == 6)
    try:
        session.drain_initial_input()
        if args.launch_only:
            pass
        else:
            if read_region(APP_BASE, 64, "live_app_check") != previous[:64]:
                raise RuntimeError("Live application changed since backup; nothing erased")
            print(json.dumps({"stage": "programming_application", **summary}), flush=True)
            summary.update(executed=True, application_write_started=True)
            send(1, dat[:28])
            payload = dat[28:]
            for offset in range(0, len(payload), 32):
                send(0, payload[offset:offset+32])
                if (offset + 32) % 4096 == 0:
                    print(json.dumps({"stage": "programmed", "bytes": offset+32, "total": len(payload)}), flush=True)
            send(6, tag)
            observed = read_region(APP_BASE, len(expected), "application_readback")
            (args.out / "installed-readback.bin").write_bytes(observed)
            if observed != expected:
                raise RuntimeError("Application readback mismatch; finish NOT sent")
            summary.update(application_readback_matches=True)
        summary.update(loader_metadata_write_commands_sent=False)
        (args.out / "result.json").write_text(json.dumps(summary, indent=2)+"\n")
        # Stock finish erases its three reserved scratch pages and launches.
        # There is no ordinary ACK; never retry an ambiguous finish.
        summary.update(executed=True, finish_requested=True)
        send(2, finish=True)
        summary.update(executed=True, finish_sent=True)
    except BaseException as exc:
        summary.update(error=str(exc))
        raise
    finally:
        (args.out / "result.json").write_text(json.dumps(summary, indent=2)+"\n")
        session.close()
    print(json.dumps(summary, indent=2), flush=True)

if __name__ == "__main__":
    main()
