#!/usr/bin/env python3
"""Read-only backup through the supported original 8BitDo HID bootloader.

Only opcode 0x31 is transmitted. Full 128 KiB, two-read matching backups
were exercised on the documented receiver. Other models and bootloader
revisions are not covered. Never unlocks or writes flash; see docs/flashing.md.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys

VID, PID = 0x0483, 0x5760
FLASH_START, FLASH_END = 0x08000000, 0x08020000  # permitted range, NOT capacity
METADATA_ADDR, APP_ADDR = 0x08003000, 0x08003400
MAX_CHUNK, REPORT_SIZE = 23, 64


class ProbeError(Exception):
    pass


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def emit(value):
    print(json.dumps(value, sort_keys=True), flush=True)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_range(address, count):
    if count <= 0 or not FLASH_START <= address < address + count <= FLASH_END:
        raise ProbeError("Read must stay within [0x08000000, 0x08020000) with positive length")


def build_read_request(address, count):
    check_range(address, count)
    if count > MAX_CHUNK:
        raise ProbeError("Read chunk exceeds the largest captured request (23 bytes)")
    request = bytearray(REPORT_SIZE)
    # Byte 0 = packet length, byte 1 = packet type; NOT a general LE16 length.
    request[0:3] = bytes((8, 0, 0x31))
    struct.pack_into("<HI", request, 3, count + 4, address)
    return bytes(request)  # all bytes after the address are deliberately zero


def parse_read_response(reply, count):
    if not 1 <= count <= MAX_CHUNK:
        raise ProbeError("Invalid expected read length")
    if len(reply) != REPORT_SIZE:
        raise ProbeError(f"Expected a 64-byte unnumbered input report; got {len(reply)}")
    if reply[0] != count + 9:
        raise ProbeError(f"Unexpected packet length {reply[0]}; expected {count + 9}")
    if reply[1] != 0 or reply[2] != 0x14:
        raise ProbeError(f"Unexpected response type/opcode: {reply[1:3].hex()}")
    if struct.unpack_from("<H", reply, 3)[0] != count + 4:
        raise ProbeError("Unexpected command length; possible stale or incompatible response")
    # The host DLL copies ONLY these count bytes. Four following bytes are not
    # part of that copied payload; their meaning is unverified. No validated
    # checksum or address echo is available to this tool.
    return bytes(reply[5:5 + count])


def device_description(info, index):
    interface = info.get("interface_number")
    usage = info.get("usage_page")
    reasons = []
    if info.get("vendor_id") != VID or info.get("product_id") != PID:
        reasons.append("VID/PID mismatch")
    if interface not in (None, -1, 0):
        reasons.append("interface is not 0")
    if usage not in (None, 0, 0x8C):
        reasons.append("usage page is not 0x8c")
    if interface != 0 and usage != 0x8C:
        reasons.append("neither expected interface nor usage page can be verified")
    path = info.get("path", b"")
    path_bytes = path if isinstance(path, bytes) else str(path).encode()
    return {
        "index": index, "vid": "0x0483", "pid": "0x5760",
        "interface_number": interface, "usage_page": usage,
        "usage": info.get("usage"), "manufacturer": info.get("manufacturer_string"),
        "product": info.get("product_string"), "serial": info.get("serial_number"),
        "path_sha256": digest(path_bytes), "eligible": not reasons,
        "rejection_reasons": reasons,
    }


def load_hid():
    try:
        import hid  # package: hidapi; imported only for hardware commands
    except ImportError as exc:
        raise ProbeError("Install the Python 'hidapi' package (it provides import hid)") from exc
    if not hasattr(hid, "device") or not hasattr(hid, "enumerate"):
        raise ProbeError("Expected the hidapi package API: hid.device and hid.enumerate")
    return hid


def enumerate_devices(hid):
    return sorted(hid.enumerate(VID, PID), key=lambda item: repr(item.get("path", "")))


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


class ReadSession:
    def __init__(self, device, log_path, timeout_ms):
        self.device, self.timeout_ms, self.sequence = device, timeout_ms, 0
        self.read_succeeded = False
        self.probe_passed = False
        self.log_file = log_path.open("x", encoding="utf-8")

    def log(self, event, **fields):
        self.log_file.write(json.dumps({"time_utc": now(), "event": event, **fields}) + "\n")
        self.log_file.flush()

    def drain_initial_input(self):
        # No writes. Remove existing queued input before the first request.
        for i in range(32):
            reply = bytes(self.device.read(REPORT_SIZE, 20))
            if not reply:
                self.log("initial_drain_complete", discarded_reports=i)
                return
            self.log("initial_queued_input", response_wire_hex=reply.hex())
        raise ProbeError("Input did not become idle; close other updater/access programs")

    def read_chunk(self, address, count, phase):
        request = build_read_request(address, count)
        output = b"\x00" + request  # HIDAPI requires report ID 0 before the 64 wire bytes
        self.sequence += 1
        seq = self.sequence
        self.log("request", sequence=seq, phase=phase, address=f"0x{address:08x}",
                 count=count, request_wire_hex=request.hex(), hid_write_hex=output.hex())
        try:
            written = self.device.write(output)  # ONLY opcode 0x31 can reach this call
            if written != len(output):
                raise ProbeError(f"HID write returned {written}; expected 65 bytes")
            reply = bytes(self.device.read(REPORT_SIZE, self.timeout_ms))
            self.log("response", sequence=seq, response_wire_hex=reply.hex(), count=len(reply))
            if not reply:
                raise ProbeError("Read timed out; no retry, reset, unlock or update will be attempted")
            data = parse_read_response(reply, count)
            self.log("accepted", sequence=seq, data_hex=data.hex(),
                     excluded_four_bytes_hex=reply[5 + count:9 + count].hex())
            return data
        except Exception as exc:
            self.log("error", sequence=seq, error=str(exc))
            raise

    def read_file(self, address, count, path, phase):
        check_range(address, count)
        partial = path.with_suffix(path.suffix + ".partial")
        completed, next_progress = 0, 4096
        with partial.open("xb") as stream:
            while completed < count:
                chunk = self.read_chunk(address + completed,
                                        min(MAX_CHUNK, count - completed), phase)
                stream.write(chunk)
                stream.flush()  # retain every accepted chunk if a later request fails
                completed += len(chunk)
                if completed >= next_progress:
                    emit({"event": "progress", "phase": phase,
                          "bytes": completed, "total": count})
                    next_progress += 4096
        os.replace(partial, path)
        return path.read_bytes()

    def close(self):
        try:
            self.device.close()
        finally:
            self.log_file.close()


def vector_check(data, label):
    if len(data) < 8:
        raise ProbeError(f"{label}: short vector table")
    sp, reset = struct.unpack_from("<II", data)
    if sp & 3 or not 0x20000000 < sp <= 0x20005000:
        raise ProbeError(f"{label}: implausible initial SP 0x{sp:08x} for nominal 20 KiB SRAM")
    lo, hi = (FLASH_START, METADATA_ADDR) if label == "boot" else (APP_ADDR, FLASH_END)
    if not reset & 1 or not lo <= (reset & ~1) < hi:
        raise ProbeError(f"{label}: implausible Thumb reset vector 0x{reset:08x}")
    return {"initial_sp": f"0x{sp:08x}", "reset_vector": f"0x{reset:08x}",
            "sha256": digest(data), "plausible": True}


def compare_reference_header(observed, reference):
    if len(observed) != 32 or len(reference) != 32:
        raise ProbeError("App header comparison requires exactly 32 bytes")
    observed_id, reference_id = (struct.unpack_from("<I", data, 0x1C)[0]
                                 for data in (observed, reference))
    prefix_matches = observed[:0x1C] == reference[:0x1C]
    exact = observed == reference
    # SET_ENCODE_ID personalizes this reserved vector word after installation.
    # Only an erased reference placeholder permits a different observed word.
    accepted = prefix_matches and (observed_id == reference_id or reference_id == 0xFFFFFFFF)
    result = ("matched_exactly" if exact else
              "matched_except_installation_id" if accepted else "mismatch")
    return {"accepted": accepted, "result": result,
            "first_28_bytes_match": prefix_matches, "exact_32_bytes_match": exact,
            "installation_id_offset": "0x1c",
            "observed_installation_id": f"0x{observed_id:08x}",
            "reference_installation_id": f"0x{reference_id:08x}",
            "reference_has_erased_placeholder": reference_id == 0xFFFFFFFF}


def run_probe(session, directory, status, reference_header=None):
    results = {}
    for label, address, count in (("metadata", METADATA_ADDR, 40),
                                  ("boot", FLASH_START, 32), ("app", APP_ADDR, 32)):
        samples = []
        for repetition in (1, 2):
            phase = f"probe_{label}_pass{repetition}"
            status["phase"] = phase
            save_json(directory / "status.json", status)
            samples.append(session.read_file(address, count, directory / f"{phase}.bin", phase))
        if samples[0] != samples[1]:
            raise ProbeError(f"Repeated {label} reads differ; aborting")
        results[label] = {"address": f"0x{address:08x}", "bytes": count,
                          "sha256": digest(samples[0]), "repeated_reads_match": True}
        status["probe"] = results
        if label == "metadata":
            session.read_succeeded = True
            status["session_read_succeeded"] = True
            status["standalone_read_succeeded"] = True
        if label != "metadata":
            results[label]["vectors"] = vector_check(samples[0], label)
        if label == "app" and reference_header is not None:
            check = compare_reference_header(samples[0], reference_header)
            results[label]["reference_header_check"] = check
            save_json(directory / "status.json", status)
            if not check["first_28_bytes_match"]:
                raise ProbeError("App header bytes 0x00..0x1b do not match the supplied reference")
            if not check["accepted"]:
                raise ProbeError("App installation-ID word differs from the programmed reference; "
                                 "only reference 0xFFFFFFFF is treated as a placeholder")
        save_json(directory / "status.json", status)
    session.probe_passed = True
    status["probe_passed"] = True
    save_json(directory / "status.json", status)
    return results


def run_dump(session, directory, status, address, count):
    # This function is deliberately unavailable until this session passed a probe.
    if not session.probe_passed or not status.get("probe_passed"):
        raise ProbeError("A successful probe in this connection is required before a dump")
    paths = []
    for repetition in (1, 2):
        phase = f"flash_pass{repetition}"
        status["phase"] = phase
        save_json(directory / "status.json", status)
        path = directory / f"{phase}.bin"
        session.read_file(address, count, path, phase)
        paths.append(path)
    first, second = (path.read_bytes() for path in paths)
    if first != second:
        mismatch = next(i for i, (a, b) in enumerate(zip(first, second)) if a != b)
        status["first_mismatch_address"] = f"0x{address + mismatch:08x}"
        raise ProbeError("Two complete flash reads differ; no verified backup was produced")
    comparisons = {}
    for label, details in status["probe"].items():
        probe_address = int(details["address"], 16)
        probe_data = (directory / f"probe_{label}_pass1.bin").read_bytes()
        lo = max(address, probe_address)
        hi = min(address + count, probe_address + len(probe_data))
        if lo >= hi:
            continue
        expected = probe_data[lo - probe_address:hi - probe_address]
        if first[lo - address:hi - address] != expected:
            status["mismatched_probe_span"] = label
            raise ProbeError(f"Dump disagrees with earlier {label} probe; no final backup produced")
        comparisons[label] = {"bytes_compared": hi - lo, "matches": True}
    final = directory / "flash_backup.bin"
    temporary = directory / "flash_backup.bin.tmp"
    shutil.copyfile(paths[0], temporary)
    os.replace(temporary, final)
    status["dump"] = {"start": f"0x{address:08x}", "bytes": count,
                      "sha256": digest(first), "two_full_reads_match": True,
                      "probe_span_comparisons": comparisons,
                      "file": str(final), "capacity_verified": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="List candidate devices; sends no reports")
    for command in ("probe", "dump"):
        child = sub.add_parser(command)
        child.add_argument("--device-index", type=int, required=True,
                           help="Explicit index from a fresh list command")
        child.add_argument("--path-sha256", help="Optional exact path hash from list; rejects index drift")
        child.add_argument("--out", type=Path, required=True, help="New output directory; must not exist")
        child.add_argument("--timeout-ms", type=int, default=3000)
        child.add_argument("--reference-app", type=Path,
                           help="Decrypted app at 0x08003400; first 28 bytes must match, "
                                "last word may differ only if reference is 0xFFFFFFFF")
        if command == "dump":
            child.add_argument("--start", type=lambda s: int(s, 0), default=FLASH_START)
            child.add_argument("--length", type=lambda s: int(s, 0), required=True,
                               help="Explicit requested byte count, e.g.0x20000; not an MCU capacity claim")
    args = parser.parse_args(argv)
    session, device, status = None, None, None
    try:
        if args.command != "list":
            if not 100 <= args.timeout_ms <= 60000:
                raise ProbeError("timeout-ms must be between100 and60000")
            if args.command == "dump":
                check_range(args.start, args.length)
            reference = args.reference_app.read_bytes()[:32] if args.reference_app else None
            if reference is not None and len(reference) != 32:
                raise ProbeError("Reference app must contain at least32 bytes")
        hid = load_hid()
        devices = enumerate_devices(hid)
        descriptions = [device_description(info, i) for i, info in enumerate(devices)]
        if args.command == "list":
            emit({"event": "devices", "devices": descriptions, "reports_sent": 0})
            return 0
        if not 0 <= args.device_index < len(devices):
            raise ProbeError("Device index is unavailable; run list again")
        selected = descriptions[args.device_index]
        if not selected["eligible"]:
            raise ProbeError("Device identity rejected: " + "; ".join(selected["rejection_reasons"]))
        if args.path_sha256 and args.path_sha256.lower() != selected["path_sha256"]:
            raise ProbeError("Device path changed; run list again and select the intended device")
        args.out.mkdir(parents=True, exist_ok=False)
        status = {"state": "running", "started_utc": now(), "command": args.command,
                  "device": selected, "probe_passed": False,
                  "only_transmitted_opcode": "0x31", "author_hardware_tested": False,
                  "session_read_succeeded": False, "standalone_read_succeeded": False,
                  "capacity_verified": False,
                  "arbitrary_range_validated": False, "read_protection_behavior_validated": False}
        save_json(args.out / "status.json", status)
        emit({"event": "starting", "device": selected, "out": str(args.out),
              "warning": "Experimental readback; full-range/protection behavior untested"})
        device = hid.device()
        device.open_path(devices[args.device_index]["path"])
        device.set_nonblocking(0)
        session = ReadSession(device, args.out / "raw_requests_replies.jsonl", args.timeout_ms)
        session.drain_initial_input()
        run_probe(session, args.out, status, reference)
        if args.command == "dump":
            run_dump(session, args.out, status, args.start, args.length)
        status.update(state="complete", phase="complete", completed_utc=now())
        save_json(args.out / "status.json", status)
        emit({"event": "complete", "result": status})
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        error = str(exc) or type(exc).__name__
        if status is not None:
            status.update(state="failed", error=error, failed_utc=now())
            save_json(args.out / "status.json", status)
        emit({"event": "error", "error": error,
              "partial_files_retained": status is not None})
        return 1
    finally:
        if session is not None:
            session.close()
        elif device is not None:
            device.close()


if __name__ == "__main__":
    sys.exit(main())
