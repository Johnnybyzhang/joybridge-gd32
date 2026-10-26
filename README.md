# JoyBridge-GD32

Focused Nintendo Pro Controller and paired Joy-Con Bluetooth-to-USB firmware for the original **GD32F103C8T6 + RDA5876** 8BitDo receiver. Keeps the existing bootloader and supports offline saved pairing.

Independent community project; not affiliated with Nintendo, 8BitDo, GigaDevice, or RDA. This source release includes no stock firmware, bootloader image, device backup, or prebuilt firmware.

## Current release

Firmware **0x10029**, experimental hardware-specific source release. A Pro Controller was measured at **88.85 fresh reports/s**, averaging **11.25 ms**, with and without gyro. USB offers fresh samples at the next 1 ms host poll; it does not generate a fixed-rate stream of duplicate input. 125 Hz fresh Bluetooth input has **not** been achieved.

Two Joy-Cons have been verified connecting simultaneously, reconnecting with ordinary buttons, and reconnecting after profile switches on the preceding SDK baseline. The latest scan policy is covered by host tests; repeat the physical two-Joy-Con acceptance after building this revision. Pro rumble and gyro forwarding have been exercised; compatibility with every game or host is not established.

## Controls

| Profile | Controller | LED |
| --- | --- | --- |
| 1 | Pro Controller | Blue |
| 2 | Left + right Joy-Con, presented as one controller | Green |

- Hold receiver button 2 seconds: start pairing, including when already paired.
- Single tap during pairing: cancel and preserve the saved pairing.
- Hold 2 seconds again while pairing: delete the selected profile; deletes both Joy-Cons.
- Double tap: switch profiles.
- Pairing replaces the Pro or the corresponding Joy-Con side. An empty Joy-Con profile commits once both halves are acquired.
- Hold Capture + L + R for one second: switch the paired Joy-Cons' gyro source. Gyro remains available when the host requests it.

Connected LED is dim; pairing blinks; missing controllers produce brief pulses. Page scanning stops when the selected profile is fully ready and resumes for pairing or a missing controller. A fresh sample from either Joy-Con updates the merged USB state. Under USB backpressure the latest state wins; this is not a lossless packet recorder. One neutral safety report releases stale controls.

USB identities: Pro `057E:2009`, paired Joy-Con profile `057E:200E`, disconnected maintenance interface `057E:20FF`. A standard HID Battery Strength feature reports battery percentage (the lower connected Joy-Con battery). The idle identity exposes no gamepad controls.

## Build, flash, measure

Start with [build instructions](docs/build.md), then [bootloader flashing](docs/flashing.md). Hardware and measured limitations are in [hardware notes](docs/hardware.md).

Use Astral **uv**. The supported development path is Windows/PowerShell with MSYS2 UCRT64 GCC and Arm GCC, installed GigaDevice/Arm packs, and the official GD32 USB SDK.

```powershell
uv sync --locked --python 3.12
# Obtain dependencies and generate private radio tables as documented first.
uv run --offline python tools/build.py
uv run --offline pytest -q
```

For a connected Pro, move both sticks throughout each 30-second phase:

```powershell
uv run --offline python tools/benchmark_reports.py --seconds 30 --imu both --out build/forward-rate.json
```

This measures Windows HID read completion and independent receiver Bluetooth counters, not on-air timestamps. Raw reports remain local and should not be published without inspection.

## Licensing

Original project code is **MIT**. Included 8CryptDo code retains AeroModes' MIT notice. Dependencies have separate terms: notably BTstack's noncommercial license and the RDA source's restrictive notice. This is not a claim that the complete linked firmware is MIT or commercially licensed. See [third-party notices](THIRD_PARTY.md).
