# Building from source

## Requirements

- [uv](https://docs.astral.sh/uv/) (installs Python 3.12 and the test dependencies)
- A host C compiler available as `gcc` (Apple's `gcc`/clang works; set `CC` to use something else)
- Arm GNU toolchain: `arm-none-eabi-gcc`, `objcopy`, `size`, `nm`
  - macOS: `brew install --cask gcc-arm-embedded`
  - Debian/Ubuntu: `apt install gcc-arm-none-eabi`
  - Windows: MSYS2 UCRT64 `mingw-w64-ucrt-x86_64-gcc` and `mingw-w64-ucrt-x86_64-arm-none-eabi-gcc`. `C:/msys64/ucrt64/bin` is searched if the tools are not on `PATH`.

## Dependencies

All dependencies are git submodules, so a recursive clone fetches everything:

```sh
git clone --recursive https://github.com/Johnnybyzhang/joybridge-gd32
# or, in an existing clone:
git submodule update --init
```

| Path | Source |
| --- | --- |
| `third_party/btstack` | BTstack `51e11ce` (v1.8.3) |
| `third_party/CMSIS_6` | Arm CMSIS 6.3.0 |
| `vendor` | [joybridge-gd32-vendor](https://github.com/Johnnybyzhang/joybridge-gd32-vendor): GD32F10x DFP 2.0.3, GD32F10x Firmware Library 2.7.0 (USBD), RDA R12 init source and receiver board tables, receiver research notes |

## Build and test

```sh
uv sync
uv run tools/build.py
uv run pytest -q
```

`build.py` builds and runs the native bridge, Nintendo protocol and real-BTstack host tests. It then generates `firmware/generated/rda_tables.h` from `vendor/`, builds the firmware, and builds the ARM fixtures that the pytest emulator tests use. Use `--host-only` to run just the native tests. If the ARM build is missing, `pytest` runs `build.py` first.

The output to flash is `build/joybridge.bin`.

## Reproducing the release binary

The 0x10029 release image has SHA-256 `7a443f7c3b88436b6a54c04f3707e09e66fb6ffe32509bd9f3154a06c1bab732`. It was built with MSYS2 UCRT64 `arm-none-eabi-gcc` 13.4.0. The sources produce identical code on other toolchains, but the image bytes differ. For example, Arm GNU Toolchain 13.3.Rel1 emits a 152-byte-smaller image because its linker merges duplicate string constants and its compiler emits a slightly smaller `gap_inquiry_stop`.

## Other tools

- `tools/generate_rda_tables.py` regenerates the radio tables on its own.
- `third_party/8cryptdo/cryptdo8.py` decrypts and encrypts 8BitDo `.dat` update files.
