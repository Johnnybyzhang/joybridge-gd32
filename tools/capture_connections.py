"""Read connection/USB identity changes without changing controller state.

Reopens the maintenance HID on every sample so USB profile changes are expected.
No timing, scan, pairing, or report-mode commands are sent.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import time
from diagnostic_hid import exchange, receiver_devices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    import hid
    args.out.parent.mkdir(parents=True, exist_ok=True)
    result = {"samples": [], "trace": []}
    start = time.monotonic()
    cursor = 0xffff
    while time.monotonic() - start < args.seconds:
        row = {"seconds": round(time.monotonic() - start, 3)}
        handle = None
        try:
            devices = receiver_devices()
            if len(devices) != 1:
                raise RuntimeError(f"Enumerating: {len(devices)} maintenance devices")
            device = devices[0]
            row.update(pid=f"{device['product_id']:04x}", usage_page=device.get("usage_page"),
                       usage=device.get("usage"))
            handle = hid.device()
            handle.open_path(device["path"])
            row["ping"] = exchange(handle, 0xf0)
            row["host"] = exchange(handle, 0xf4)
            row["controllers"] = exchange(handle, 0xfa, 6)
            row["links"] = exchange(handle, 0xfa, 3)
            row.update(exchange(handle, 0xfa, 7))
            row["rate"] = exchange(handle, 0xfa, 0)
            for _ in range(24):
                trace = exchange(handle, 0xf7, cursor)
                result["trace"].extend(trace["records"])
                cursor = (trace["cursor"] + len(trace["records"])) & 0xffff
                if cursor == trace["end"] or not trace["records"]:
                    break
            peers = row["controllers"]["peers"]
            state = " ".join(f"{p['index']}:{'ready' if p['ready'] else p['connection_state']}"
                             f"/reports={p['full_reports']}" for p in peers)
            print(f"{row['seconds']:6.1f}s PID={row['pid']} {state} error={row['host']['error']}", flush=True)
        except (OSError, RuntimeError) as exc:
            row["enumeration"] = str(exc)
            print(f"{row['seconds']:6.1f}s {exc}", flush=True)
        finally:
            if handle is not None:
                handle.close()
        result["samples"].append(row)
        args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        time.sleep(.5)
    print(f"Saved {args.out}", flush=True)


if __name__ == "__main__":
    main()
