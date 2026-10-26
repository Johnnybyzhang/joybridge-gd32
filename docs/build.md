# Building from source

The published source deliberately excludes proprietary input files. Keep a local copy of the owner's stock firmware and backup before replacing the application.

## Dependencies

Install Astral uv, MSYS2 UCRT64 GCC and `arm-none-eabi-gcc`/binutils. The scripts locate tools on PATH, then use `C:/msys64/ucrt64/bin` as fallback. Install GigaDevice GD32F10x_DFP **2.0.3** and Arm CMSIS **6.3.0** with the Arm/Keil pack manager. `CMSIS_PACK_ROOT`, `GD32_DFP_ROOT`, and `ARM_CMSIS_ROOT` can override installed paths.

Obtain official **GD32F10x Firmware Library 2.7.0**, unpack it at `third_party/gd32f10x/GD32F10x_Firmware_Library_V2.7.0`, or set `GD32_FIRMWARE_ROOT` to its root. The build needs `Firmware/GD32F10x_usbd_library`, not USBFS. See [notices](../THIRD_PARTY.md).

```powershell
git clone https://github.com/bluekitchen/btstack third_party/btstack
git -C third_party/btstack checkout 51e11ce3692abfe31b4ee3d66cffdeb1ae1626f2
uv sync --locked --python 3.12
```

## Local radio tables

Supply the pinned RDA source listed in THIRD_PARTY.md as `private/rdabt_r12_init.c`. The generator verifies its Git blob identity. Supply your legally obtained stock v2.05 update as `private/v2.05.dat` and decrypt locally with the included MIT tool:

```powershell
uv run --offline python third_party/8cryptdo/cryptdo8.py decrypt private/v2.05.dat -c gd32 -o private/v2.05_decrypted.bin
uv run --offline python tools/generate_rda_tables.py --stock-app private/v2.05_decrypted.bin --rda-source private/rdabt_r12_init.c
```

The supported decrypted application SHA-256 is `36817322fc13106cb0663b6d84edb8f3fb35dd6b94722f514675e83350b84b83`. Different firmware layouts are rejected, not parsed using guessed offsets. The generated header is private build output and is ignored by Git.

```powershell
uv run --offline python tools/build.py
uv run --offline pytest -q
```

`build.py` runs native bridge, Nintendo protocol and real-BTstack host tests, then builds the ARM application and SDK fixtures. The flashable output is `build/hid-diagnostic.bin` plus its matching JSON manifest. Historical diagnostic names remain to preserve tool compatibility.

`research/` contains archived offline tests that require the original private analysis bundles. They are not part of the public default test run. Older experimental probe commands remain for research and are rejected by current firmware where retired.
