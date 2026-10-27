# Flashing

JoyBridge installs through the receiver's original 8BitDo USB bootloader using ordinary application-update packets. The bootloader is never modified, and you can return to stock firmware with 8BitDo's updater at any time.

Supported hardware is the GD32F103C8T6 + RDA5876 receiver whose bootloader code (`0x08000000`–`0x08002FFF`) has SHA-256 `8d733de72baf8cc04ce3ada92e3df39be405b55587a749514c0c756eaad38692`. `bootloader.py` checks this before writing. If you have a different receiver, check its flash layout before passing `--force`.

## Enter the bootloader

- From stock firmware or a blank receiver: hold the receiver button while plugging it in. It enumerates as `0483:5760`.
- From JoyBridge: `uv run tools/diagnostic_hid.py --bootloader`

## Flash

```sh
uv run tools/bootloader.py flash                  # build/joybridge.bin
uv run tools/bootloader.py flash path/to/image.bin
```

The tool reads the device's 12-byte installation stamp (flash `0x0800341C`), which the bootloader writes back after programming. It saves a copy to `private/device-tag.hex`. It then programs the image, reads it back to verify, and starts it. If verification fails the bootloader stays active, so you can simply flash again.

If an interrupted update erased the stamp, the tool uses `private/device-tag.hex`. Without that file, pass the stamp yourself with `--tag <24 hex digits>`, taking it from offset `0x341C` of a flash backup.

## Backup and launch

```sh
uv run tools/bootloader.py backup                 # private/flash-backup.bin (full 128 KiB)
uv run tools/bootloader.py launch                 # leave the bootloader without flashing
```

`launch` sends the bootloader's normal finish command. That command erases the bootloader's three scratch pages and starts the installed application.

## Back to stock

Run 8BitDo's updater, or package a stock image yourself. Stock update files are in `vendor/stock/`.
