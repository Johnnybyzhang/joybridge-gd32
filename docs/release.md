# Source release 0.1.0 / firmware 0x10029

Prepared from the hardware-tested forwarding revision. Original code: MIT.

Validation of the standalone publication tree:

- Native bridge, Nintendo and real-BTstack host tests passed.
- ARM application and official-SDK fixture build passed.
- 13 Python tests passed, including ARM USB fixture execution.
- Generated radio arrays from locally supplied inputs reproduced the working firmware exactly.
- Firmware SHA-256: `7a443f7c3b88436b6a54c04f3707e09e66fb6ffe32509bd9f3154a06c1bab732`.
- Public flasher offline packaging/preflight passed using a local owner backup; update wire protocol unchanged. No new hardware flash was performed during release preparation.

The public flasher now locates bundled MIT packaging code and the public readback helper, and identifies the supported bootloader by its code-region digest rather than a private reference file. Unsupported bootloaders remain rejected.

The source archive is made from the Git index, excluding ignored build/dependency/private files. This repository contains source only; no prebuilt firmware release is distributed.
