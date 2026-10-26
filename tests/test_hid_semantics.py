"""Decode the declared generic HID controls, independently of Nintendo HIDAPI."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def descriptor():
    source = (ROOT / "firmware/src/pro_usb.c").read_text()
    body = source.split("pro_usb_report_descriptor[] = {", 1)[1].split("};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", body))


def parse_hid(data):
    globals_, local, offsets, fields, collections = {}, [], {}, [], []
    position, minimum = 0, None
    while position < len(data):
        prefix = data[position]
        position += 1
        assert prefix != 0xfe, "No long HID items expected"
        size = (0, 1, 2, 4)[prefix & 3]
        value = int.from_bytes(data[position:position + size], "little")
        position += size
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        if kind == 1:
            globals_[tag] = value
        elif kind == 2:
            if tag == 0:
                local.append(value if size == 4 else globals_.get(0, 0) << 16 | value)
            elif tag == 1:
                minimum = value
            elif tag == 2:
                assert minimum is not None
                local.extend(globals_.get(0, 0) << 16 | u for u in range(minimum, value + 1))
        elif kind == 0:
            if tag == 10 and value == 1:
                collections.append(local[0])
            if tag in (8, 9, 11):
                report = globals_.get(8, 0)
                key = tag, report
                offset = offsets.get(key, 0)
                bits, count = globals_[7], globals_[9]
                for i in range(count):
                    usage = local[min(i, len(local) - 1)] if local else 0
                    fields.append((tag, report, offset, bits, bool(value & 1), usage, globals_.copy()))
                    offset += bits
                offsets[key] = offset
            local, minimum = [], None
    return fields, offsets, collections


def generic_controls(report):
    fields, _, _ = parse_hid(descriptor())
    payload = int.from_bytes(report[1:], "little")
    buttons, axes = set(), {}
    for tag, ident, offset, bits, constant, usage, _ in fields:
        if tag != 8 or ident != report[0] or constant:
            continue
        value = payload >> offset & ((1 << bits) - 1)
        page, item = usage >> 16, usage & 0xffff
        if page == 9 and value:
            buttons.add(item)
        if page == 1 and item in (0x30, 0x31, 0x32, 0x35, 0x39):
            axes[item] = value
    return buttons, axes


def nintendo_report(timer=0, battery=0x81, buttons=0, sticks=(2048,) * 4):
    report = bytearray(64)
    report[:3] = bytes((0x30, timer, battery))
    report[3:6] = buttons.to_bytes(3, "little")
    for offset, x, y in ((6, *sticks[:2]), (9, *sticks[2:])):
        report[offset:offset + 3] = (x | y << 12).to_bytes(3, "little")
    # Telemetry and IMU are allowed to vary independently of the controls.
    report[12:] = bytes((timer * 7 + i * 31) & 255 for i in range(52))
    return report


def test_changing_timer_battery_and_imu_never_generates_controls():
    for timer in range(256):
        for battery in (1, 0x21, 0x81, 0x91):
            buttons, axes = generic_controls(nintendo_report(timer, battery))
            assert not buttons, (timer, battery, buttons)
            assert axes == {0x30: 2048, 0x31: 2048, 0x32: 2048, 0x35: 2048}


def test_actual_buttons_and_packed_sticks_decode_correctly():
    # Nintendo wire bits: Y X B A, R ZR, minus plus R3 L3 home capture,
    # down up right left, L ZL. Rail and reserved bits are not generic buttons.
    bits = (0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 22, 23)
    for usage, bit in enumerate(bits, 1):
        buttons, axes = generic_controls(nintendo_report(buttons=1 << bit, sticks=(500, 1500, 2500, 3500)))
        assert buttons == {usage}
        assert axes == {0x30: 500, 0x31: 1500, 0x32: 2500, 0x35: 3500}
    reserved = sum(1 << bit for bit in set(range(24)) - set(bits))
    assert generic_controls(nintendo_report(buttons=reserved))[0] == set()


def test_nintendo_lengths_collection_and_non_gamepad_reports():
    _, lengths, collections = parse_hid(descriptor())
    assert collections == [0x00010004]  # SDL's Nintendo HIDAPI selection.
    assert lengths == {(8, 0x30): 504, (8, 0x21): 504, (8, 0x81): 504,
                       (9, 1): 504, (9, 0x10): 504, (9, 0x80): 504, (9, 0x82): 504,
                       (11, 0x90): 8}
    for report in (0x21, 0x81):
        assert generic_controls(bytes([report]) + b"\xff" * 63) == (set(), {})


def test_standard_hid_battery_strength_is_a_percentage_feature():
    fields, _, _ = parse_hid(descriptor())
    battery = [f for f in fields if not f[4] and f[5] == 0x00060020]
    assert len(battery) == 1
    tag, report, offset, bits, _, _, g = battery[0]
    assert (tag, report, offset, bits) == (11, 0x90, 0, 8)
    assert (g[1], g[2], g[3], g[4]) == (0, 100, 0, 100)
