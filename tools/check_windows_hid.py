"""Inspect the receiver using Windows' actual HID descriptor parser.

Synthetic reports are parsed locally, never sent to the device or Windows input.
Live sampling is read-only; it does not change Bluetooth, pairing or USB mode.
"""
import argparse
import ctypes as c
import json
from pathlib import Path
import struct
import time
import hid
from diagnostic_hid import receiver_devices


def check(status):
    if status < 0:
        raise RuntimeError(f"HID parser status 0x{status & 0xffffffff:08x}")


class WindowsHid:
    def __init__(self, path):
        self.kernel = c.WinDLL("kernel32", use_last_error=True)
        self.hid = c.WinDLL("hid")
        self.kernel.CreateFileW.argtypes = [c.c_wchar_p, c.c_ulong, c.c_ulong, c.c_void_p,
                                          c.c_ulong, c.c_ulong, c.c_void_p]
        self.kernel.CreateFileW.restype = c.c_void_p
        self.kernel.CloseHandle.argtypes = [c.c_void_p]
        self.hid.HidD_GetPreparsedData.argtypes = [c.c_void_p, c.POINTER(c.c_void_p)]
        self.hid.HidD_FreePreparsedData.argtypes = [c.c_void_p]
        self.hid.HidD_GetFeature.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
        self.hid.HidD_GetFeature.restype = c.c_ubyte
        self.hid.HidP_GetCaps.argtypes = [c.c_void_p, c.c_void_p]
        self.hid.HidP_GetValueCaps.argtypes = [c.c_int, c.c_void_p, c.POINTER(c.c_ushort), c.c_void_p]
        self.hid.HidP_GetUsages.argtypes = [c.c_int, c.c_ushort, c.c_ushort, c.c_void_p,
                                          c.POINTER(c.c_ulong), c.c_void_p, c.c_void_p, c.c_ulong]
        for name in ("HidP_GetUsageValue", "HidP_GetScaledUsageValue"):
            getattr(self.hid, name).argtypes = [c.c_int, c.c_ushort, c.c_ushort, c.c_ushort,
                                              c.c_void_p, c.c_void_p, c.c_void_p, c.c_ulong]
        self.handle = self.kernel.CreateFileW(path.decode(), 0, 3, None, 3, 0, None)
        if self.handle == c.c_void_p(-1).value:
            raise c.WinError(c.get_last_error())
        self.preparsed = c.c_void_p()
        if not self.hid.HidD_GetPreparsedData(self.handle, c.byref(self.preparsed)):
            self.kernel.CloseHandle(self.handle)
            raise OSError("Cannot read HID preparsed data")

    def close(self):
        self.hid.HidD_FreePreparsedData(self.preparsed)
        self.kernel.CloseHandle(self.handle)

    def capabilities(self):
        caps = c.create_string_buffer(64)
        check(self.hid.HidP_GetCaps(self.preparsed, caps))
        words = struct.unpack("<32H", caps.raw)
        values = []
        for report_type, cap_index in ((0, 24), (2, 30)):
            count = c.c_ushort(words[cap_index])
            if not count.value:
                continue
            data = c.create_string_buffer(count.value * 72)
            check(self.hid.HidP_GetValueCaps(report_type, data, c.byref(count), self.preparsed))
            for i in range(count.value):
                row = data.raw[i * 72:(i + 1) * 72]
                values.append({"type": report_type, "page": struct.unpack_from("<H", row)[0], "report_id": row[2],
                               "usage": struct.unpack_from("<H", row, 56)[0],
                               "bits": struct.unpack_from("<H", row, 18)[0],
                               "logical": list(struct.unpack_from("<ii", row, 40)),
                               "physical": list(struct.unpack_from("<ii", row, 48))})
        return {"usage_page": words[1], "usage": words[0], "input_bytes": words[2],
                "output_bytes": words[3], "feature_bytes": words[4], "button_caps": words[23], "values": values}

    def controls(self, report):
        data = c.create_string_buffer(bytes(report), len(report))
        usages, count = (c.c_ushort * 64)(), c.c_ulong(64)
        check(self.hid.HidP_GetUsages(0, 9, 0, usages, c.byref(count), self.preparsed, data, len(report)))
        def value(page, usage, scaled=False):
            output = c.c_long() if scaled else c.c_ulong()
            fn = self.hid.HidP_GetScaledUsageValue if scaled else self.hid.HidP_GetUsageValue
            check(fn(0, page, 0, usage, c.byref(output), self.preparsed, data, len(report)))
            return output.value
        return {"buttons": sorted(usages[:count.value]),
                "axes": [value(1, usage) for usage in (0x30, 0x31, 0x32, 0x35)]}

    def battery(self, simulated_percent=None):
        data = c.create_string_buffer(bytes((0x90, simulated_percent or 0)), 2)
        if simulated_percent is None and not self.hid.HidD_GetFeature(self.handle, data, 2):
            raise OSError("Cannot read standard HID Battery Strength feature")
        raw, scaled = c.c_ulong(), c.c_long()
        check(self.hid.HidP_GetUsageValue(2, 6, 0, 0x20, c.byref(raw), self.preparsed, data, 2))
        check(self.hid.HidP_GetScaledUsageValue(2, 6, 0, 0x20, c.byref(scaled), self.preparsed, data, 2))
        assert raw.value == scaled.value and 0 <= raw.value <= 100
        return raw.value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error("--seconds must not be negative")
    devices = receiver_devices()
    if len(devices) != 1:
        raise RuntimeError(f"Expected one receiver, found {len(devices)}")
    device = devices[0]
    windows = WindowsHid(device["path"])
    result = {"pid": f"{device['product_id']:04x}", "capabilities": windows.capabilities()}
    handle = None
    try:
        if device["product_id"] == 0x20ff:
            assert result["capabilities"]["usage_page"] == 0xff00
            result["vendor_only_idle"] = True
            return
        for timer in range(256):
            for level in range(5):
                report = bytearray([0x30, timer, level << 5 | 1]) + bytearray(61)
                report[6:12] = bytes.fromhex("00 08 80 00 08 80")
                report[12:] = bytes((timer + i * 37) & 255 for i in range(52))
                sample = windows.controls(report)
                assert sample == {"buttons": [], "axes": [2048] * 4}, sample
        result["windows_neutral_cases_passed"] = 1280
        for percent in range(101):
            assert windows.battery(percent) == percent
        result["windows_battery_cases_passed"] = 101
        result["battery_feature_percent"] = windows.battery()
        handle = hid.device()
        handle.open_path(device["path"])
        samples, ignored = [], {}
        until = time.monotonic() + args.seconds
        while time.monotonic() < until:
            report = bytes(handle.read(64, 100))
            if len(report) == 64 and report[0] == 0x30:
                sample = windows.controls(report)
                sample["nintendo_buttons"] = report[3:6].hex()
                sample["battery_percent"] = (report[2] >> 5) * 25
                samples.append(sample)
            elif report:
                key = f"{report[0]:02x}"
                ignored[key] = ignored.get(key, 0) + 1
        result["live_reports"] = len(samples)
        result["other_reports"] = ignored
        if samples:
            result["button_states"] = sorted({tuple(s["buttons"]) for s in samples})
            result["nintendo_button_states"] = sorted({s["nintendo_buttons"] for s in samples})
            result["axes_min"] = [min(s["axes"][i] for s in samples) for i in range(4)]
            result["axes_max"] = [max(s["axes"][i] for s in samples) for i in range(4)]
            result["battery_percent"] = sorted({s["battery_percent"] for s in samples})
    finally:
        if handle is not None:
            handle.close()
        windows.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
