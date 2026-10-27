# JoyBridge-GD32

Focused Nintendo Pro Controller and paired Joy-Con Bluetooth-to-USB firmware for the original **GD32F103C8T6 + RDA5876** 8BitDo receiver. Keeps the existing bootloader and supports offline saved pairing.

Independent community project; not affiliated with Nintendo, 8BitDo, GigaDevice, or RDA.

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

## Build and flash

You need [uv](https://docs.astral.sh/uv/), a host C compiler (`gcc` or `clang` as `gcc`) and the Arm GNU toolchain (`arm-none-eabi-gcc`) on `PATH`. Builds work on macOS, Linux and Windows (MSYS2 UCRT64).

```sh
git clone --recursive https://github.com/Johnnybyzhang/joybridge-gd32
cd joybridge-gd32
uv sync
uv run tools/build.py      # host tests + build/joybridge.bin
uv run pytest -q           # emulator and protocol tests
```

Put the receiver in bootloader mode (hold its button while plugging it in), then:

```sh
uv run tools/bootloader.py flash
```

More detail is in [building](docs/build.md), [flashing](docs/flashing.md) and the [hardware notes](docs/hardware.md).

To measure input rate with a Pro Controller connected (keep moving both sticks):

```sh
uv run tools/benchmark_reports.py --seconds 30 --imu both --out build/forward-rate.json
```

This measures HID read completion on the host together with the receiver's own Bluetooth counters. It does not capture on-air timestamps.

## Licensing

Original project code is **MIT**. Dependencies keep their own terms: BTstack is licensed for noncommercial use only, and the RDA, GigaDevice and 8BitDo files in the `vendor` submodule keep their original notices. As a result, the complete linked firmware is not MIT-licensed. See [third-party notices](THIRD_PARTY.md).
