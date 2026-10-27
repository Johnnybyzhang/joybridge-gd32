# Release 0.1.0 / firmware 0x10029

- Firmware SHA-256 `7a443f7c3b88436b6a54c04f3707e09e66fb6ffe32509bd9f3154a06c1bab732`. It was built with MSYS2 UCRT64 `arm-none-eabi-gcc` 13.4.0; see [build notes](build.md#reproducing-the-release-binary).
- The native bridge, Nintendo protocol and real-BTstack host tests pass, as do the pytest emulator suite, the stock bootloader update emulation and the RDA initialization trace.
- The generated radio tables match those of the hardware-tested build.
