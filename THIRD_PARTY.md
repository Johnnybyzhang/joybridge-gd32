# Third-party inputs and release boundary

The root MIT license applies to original project code, not to independent dependencies or generated vendor payloads.

- **8CryptDo:** bundled under `third_party/8cryptdo`, MIT, copyright AeroModes. Its LICENSE and source notices are retained. Upstream: https://github.com/aeromodes/8cryptdo . This is source tooling, not an 8BitDo firmware binary.
- **BTstack:** obtain commit `51e11ce3692abfe31b4ee3d66cffdeb1ae1626f2` from https://github.com/bluekitchen/btstack . Its LICENSE limits redistribution/use/modification to personal benefit without commercial purpose or monetary gain. Obtain an appropriate license from BlueKitchen for commercial use. Not bundled.
- **GigaDevice / Arm:** use GD32F10x_DFP 2.0.3, CMSIS 6.3.0 and GD32F10x Firmware Library 2.7.0 under their accompanying terms. Not bundled. Official download: https://www.gd32mcu.com/download/down/document_id/180/path_type/1 . Archive SHA-256: `3494fcb21df8710335cb49dffafd6722c5f2c9dadf5c942bc5369a30ca163b4f`.
- **RDA R12 source:** `rdabt_r12_init.c` from source tree `b2b6287e8fa9a9f49b679a357b49beb1c31d821c`, Git blob `9117a02f780eefc4111fe5aab643fb5b89e98519`. Its notice reserves rights and states it conveys no license. Neither that file nor generated patch arrays are bundled. Obtain and review the source separately; availability on a public repository is not a license grant. https://github.com/cherryding1/RDA8955_W17.44_IDH/blob/b2b6287e8fa9a9f49b679a357b49beb1c31d821c/soft/platform/edrv/btd/rdabt_driver/src/rdabt_r12_init.c
- **8BitDo stock application:** owner-supplied decrypted v2.05 image is required locally to extract board initialization tables. No stock application, bootloader, decrypted code arrays, captures, or device backups are included. No redistribution rights are asserted for those inputs or their generated copies.

Python dependency versions and origins are in `uv.lock`. Native SDK provenance is recorded locally by the build. TinyUSB was explored during development but is not used or required by this release.

Do not upload `private/`, `build/`, `firmware/generated/`, SDK trees, captures, flash dumps, pairing journals, or generated firmware binaries as part of this source release. Generated output can contain vendor patch code even though its filename ends in `.h`.
