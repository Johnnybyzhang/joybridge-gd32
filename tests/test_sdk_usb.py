from __future__ import annotations
import importlib.util
from pathlib import Path
import struct
import shutil
import subprocess
import sys
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK

ROOT = Path(__file__).resolve().parents[1]
NM = shutil.which("arm-none-eabi-nm")
sys.path.insert(0, str(ROOT / "tools"))
from check_image import APP_BASE, validate
from package_image import package


def test_official_sdk_usb_enumeration_and_backpressure():
    raw = (ROOT / "build/sdk-usb-test.bin").read_bytes()
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(APP_BASE, raw)
    u.mem_map(0x20000000, 0x5000)
    u.mem_map(0x40000000, 0x30000)
    u.mem_map(0xE000E000, 0x2000)
    u.mem_map(0x1FFFF000, 0x1000)  # synthetic unique-ID registers for SDK serial
    def stop_on_wfi(uc, address, size, context):
        if bytes(uc.mem_read(address, 2)) == b"\x30\xbf":
            uc.emu_stop()
    u.hook_add(UC_HOOK_CODE, stop_on_wfi)
    u.reg_write(UC_ARM_REG_SP, 0x20004FE8)
    reset = struct.unpack_from("<I", raw, 4)[0]
    u.emu_start(reset, 0x08000000, count=200000)
    symbols = subprocess.check_output([NM, "build/sdk-usb-test.elf"], cwd=ROOT, text=True)
    addresses = {p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split())==3}
    status = struct.unpack("<I", bytes(u.mem_read(addresses["sdk_usb_test_status"], 4)))[0]
    assert status == 0x53444b59, f"SDK USB fixture failed at C line {status}"

