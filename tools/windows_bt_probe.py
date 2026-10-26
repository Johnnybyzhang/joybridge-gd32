"""Bounded Classic Bluetooth inquiry using the existing Windows radio.

Uses BluetoothFindFirstDevice (bthprops.cpl), without pairing or changing radio
settings. Only the requested receiver's details are printed or saved.
API: https://learn.microsoft.com/windows/win32/api/bluetoothapis/nf-bluetoothapis-bluetoothfindfirstdevice
"""
from __future__ import annotations
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import time


class SystemTime(c.Structure):
    _fields_ = [(name, w.WORD) for name in
                ("year", "month", "weekday", "day", "hour", "minute", "second", "millisecond")]


class DeviceInfo(c.Structure):
    _fields_ = [("size", w.DWORD), ("address", c.c_uint64), ("device_class", w.ULONG),
                ("connected", w.BOOL), ("remembered", w.BOOL), ("authenticated", w.BOOL),
                ("last_seen", SystemTime), ("last_used", SystemTime), ("name", w.WCHAR * 248)]


class SearchParams(c.Structure):
    _fields_ = [("size", w.DWORD), ("authenticated", w.BOOL), ("remembered", w.BOOL),
                ("unknown", w.BOOL), ("connected", w.BOOL), ("inquiry", w.BOOL),
                ("timeout", c.c_ubyte), ("radio", w.HANDLE)]


def inquire(address: int | None, name: str, timeout: int, cached: bool = False) -> dict:
    api = c.WinDLL("bthprops.cpl", use_last_error=True)
    api.BluetoothFindFirstDevice.argtypes = [c.POINTER(SearchParams), c.POINTER(DeviceInfo)]
    api.BluetoothFindFirstDevice.restype = w.HANDLE
    api.BluetoothFindNextDevice.argtypes = [w.HANDLE, c.POINTER(DeviceInfo)]
    api.BluetoothFindNextDevice.restype = w.BOOL
    api.BluetoothFindDeviceClose.argtypes = [w.HANDLE]
    api.BluetoothFindDeviceClose.restype = w.BOOL
    params = SearchParams(c.sizeof(SearchParams), True, True, True, True, not cached, timeout, None)
    info = DeviceInfo(); info.size = c.sizeof(DeviceInfo)
    started = time.monotonic()
    handle = api.BluetoothFindFirstDevice(c.byref(params), c.byref(info))
    matches = []
    if not handle:
        error = c.get_last_error()
        if error != 259:  # ERROR_NO_MORE_ITEMS
            raise c.WinError(error)
    else:
        try:
            while True:
                if (address is not None and info.address == address) or info.name == name:
                    matches.append({"address": f"{info.address:012x}", "name": info.name,
                                    "device_class": f"{info.device_class:06x}",
                                    "connected": bool(info.connected),
                                    "authenticated": bool(info.authenticated),
                                    "last_seen_utc": f"{info.last_seen.year:04}-{info.last_seen.month:02}-"
                                        f"{info.last_seen.day:02}T{info.last_seen.hour:02}:"
                                        f"{info.last_seen.minute:02}:{info.last_seen.second:02}Z"})
                info = DeviceInfo(); info.size = c.sizeof(DeviceInfo)
                if not api.BluetoothFindNextDevice(handle, c.byref(info)):
                    error = c.get_last_error()
                    if error != 259:
                        raise c.WinError(error)
                    break
        finally:
            api.BluetoothFindDeviceClose(handle)
    return {"classic_inquiry": not cached, "elapsed_seconds": round(time.monotonic() - started, 2),
            "target_found": bool(matches), "matches": matches}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="Target address, e.g. 0400780f9b02")
    parser.add_argument("--name", default="8BitDo Redev Diagnostic")
    parser.add_argument("--timeout", type=int, choices=range(1, 17), default=8,
                        help="Inquiry duration in 1.28 second units")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--cached",action="store_true",help="Read Windows cached connection state without RF inquiry")
    args = parser.parse_args()
    result = inquire(int(args.address.replace(":", ""), 16) if args.address else None,
                     args.name, args.timeout, args.cached)
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
