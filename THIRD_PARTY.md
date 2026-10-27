# Third-party components

The root MIT license covers original project code only.

| Component | Location | License |
| --- | --- | --- |
| 8CryptDo (AeroModes) | `third_party/8cryptdo` | MIT, see its `LICENSE`. Upstream: https://github.com/aeromodes/8cryptdo |
| BTstack `51e11ce` | `third_party/btstack` (submodule) | BlueKitchen license: personal, noncommercial use only. Commercial use requires a license from BlueKitchen. |
| Arm CMSIS 6.3.0 | `third_party/CMSIS_6` (submodule) | Apache-2.0 |
| GigaDevice GD32F10x DFP 2.0.3 and Firmware Library 2.7.0 | `vendor/gd32` | GigaDevice SLA-GD0001 plus BSD-3-Clause source headers |
| RDA R12 init source (`rdabt_r12_init.c`) | `vendor/rda5876` | RDA notice in the file. See `vendor/rda5876/SOURCE.txt` for provenance. |

`firmware/generated/rda_tables.h` is generated at build time. It contains RDA ROM patches and receiver board values from `vendor/rda5876/hci_initialization_tables.json`.
