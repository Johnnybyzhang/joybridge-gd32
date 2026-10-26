# Original-bootloader flashing

Only the tested GD32F103C8T6 + RDA5876 receiver and supported bootloader revision are covered. Do not assume that another 8BitDo receiver shares its flash layout. The upper flash region used by this board is based on physical evidence; the MCU's nominal C8 capacity alone does not establish compatibility.

The flasher uses ordinary bootloader application-update packets. It preserves the bootloader and the device installation stamp, checks the current application header, and reads back the installed application before issuing finish. It does not unlock protection or replace the bootloader. The loader's normal finish routine manages its own scratch pages.

## First installation

Enter the receiver's original bootloader using its power-on button path. Enumerate and select the intended interface (`0483:5760`):

```powershell
uv run --offline python tools/usb_readonly_backup.py list
# Replace 0 with the intended index from list; keep this backup private.
uv run --offline python tools/usb_readonly_backup.py dump --device-index 0 --length 0x20000 --out private/pre-install
```

The backup tool performs two matching full reads. Keep the backup and its `status.json` together. The flasher recognizes the tested bootloader by the hash of its code region, without needing another owner's flash dump. It also verifies the selected device path against the backup record. Unsupported revisions fail closed.

Build first, then inspect the offline summary:

```powershell
uv run --offline python tools/flash_diagnostic.py --pre-backup private/pre-install
```

Install using a new transaction directory:

```powershell
uv run --offline python tools/flash_diagnostic.py --pre-backup private/pre-install --out private/flash-01 --execute
```

## Subsequent updates

The maintenance HID provides a guarded return to the original bootloader. Reuse the established backup and the previous successful installation record; a fresh full backup is not needed on every update.

```powershell
uv run --offline python tools/diagnostic_hid.py --bootloader
uv run --offline python tools/flash_diagnostic.py --pre-backup private/pre-install --previous-install private/flash-01/expected-installed.bin --out private/flash-02 --execute
```

For rollback to an earlier custom build, pass `--image path/to/hid-diagnostic.bin` with its matching `.json` manifest and the current installation's `--previous-install`. Keep the previous custom build locally. Restoring stock firmware uses the manufacturer's updater and your own firmware files; no stock image is distributed here.

Readback failure deliberately leaves the loader active. Do not blindly retry an ambiguous finish operation. Transaction records, backup files and generated packages remain private.
