"""Locate installed official GigaDevice/Arm packs without downloading tools."""
from __future__ import annotations
import os
from pathlib import Path

GD32_VERSION = "2.0.3"
CMSIS_VERSION = "6.3.0"
FIRMWARE_VERSION = "2.7.0"

def installed_packs() -> tuple[Path, Path]:
    packs = Path(os.environ.get("CMSIS_PACK_ROOT", Path.home() / "AppData/Local/Arm/Packs"))
    gd32 = Path(os.environ.get("GD32_DFP_ROOT", packs / "GigaDevice/GD32F10x_DFP" / GD32_VERSION))
    cmsis = Path(os.environ.get("ARM_CMSIS_ROOT", packs / "ARM/CMSIS" / CMSIS_VERSION))
    for root, relative in [(gd32, "Device/Include/gd32f10x.h"), (cmsis, "CMSIS/Core/Include/core_cm3.h")]:
        if not (root / relative).is_file():
            raise SystemExit(f"Missing official SDK file: {root / relative}. Set GD32_DFP_ROOT / ARM_CMSIS_ROOT to the installed packs.")
    return gd32, cmsis


def firmware_library() -> Path:
    root = Path(os.environ.get("GD32_FIRMWARE_ROOT", Path(__file__).resolve().parents[1] /
                "third_party/gd32f10x/GD32F10x_Firmware_Library_V2.7.0"))
    if not (root / "Firmware/GD32F10x_usbd_library/device/Source/usbd_core.c").is_file():
        raise SystemExit(f"Missing official GD32 USBD SDK at {root}. See docs/build.md or set GD32_FIRMWARE_ROOT.")
    return root
