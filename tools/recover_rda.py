"""Recover RDA setup values and trace ORIGINAL init instructions offline.

Only local flash/RAM are mapped. Hardware GPIO/UART/I2C calls and delays are
stubbed; busy-loop time is skipped explicitly. This does NOT qualify timing,
radio operation, two ACL links, or patch compatibility on physical hardware.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R4, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "e2139cea93cf56bc11e8455a23b60d2c61e4d9245e2da74e801901ab7585d1a6"

def recover(variant: int) -> dict:
    flash = (ROOT / "8bitdo_bootloader_reuse_analysis/full_backup/backup-01/flash_backup.bin").read_bytes()
    if hashlib.sha256(flash).hexdigest() != EXPECTED:
        raise ValueError("Stock flash hash differs from the analyzed receiver")
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(0x08000000, flash)
    u.mem_map(0x20000000, 0x5000); u.reg_write(UC_ARM_REG_SP, 0x20003FB8)
    # Execute actual Keil scatter loading/decompression, stop BEFORE main.
    u.emu_start(0x080034ED, 0x0801641C, count=2000000)
    assert u.reg_read(UC_ARM_REG_PC) == 0x0801641C
    rf_tail = bytes(u.mem_read(0x20000536, 8))
    u.mem_write(0x2000109A, bytes([variant]))
    u.reg_write(UC_ARM_REG_SP, 0x20003FB8); u.reg_write(UC_ARM_REG_LR, 0x08002BF1)
    events = []
    # Synthetic identity; no receiver UID, real BD_ADDR, or link key in output.
    identity = bytes.fromhex("02 a1 b2 c3 d4 e5")
    loops = {
        0x08009534:(0x0800953A,UC_ARM_REG_R4,0),
        0x08009544:(0x0800954A,UC_ARM_REG_R0,0),
        0x08009550:(0x08009556,UC_ARM_REG_R4,0),
        0x08009592:(0x0800959C,UC_ARM_REG_R0,0),
        0x08009AD4:(0x08009ADC,UC_ARM_REG_R0,60001),
        0x08009B04:(0x08009B0C,UC_ARM_REG_R0,30001),
        0x08009A26:(0x08009A2E,UC_ARM_REG_R0,60001),
        0x08009A4C:(0x08009A54,UC_ARM_REG_R0,0),
        0x08009A74:(0x08009A7C,UC_ARM_REG_R0,60001),
        0x08009A9A:(0x08009AA2,UC_ARM_REG_R0,0),
        0x080165DA:(0x080165E2,UC_ARM_REG_R0,60001),
        0x08009506:(0x0800950E,UC_ARM_REG_R0,60001),
    }
    def hook(uc, pc, size, context):
        if pc in loops:
            dest, reg, value = loops[pc]
            events.append({"event":"busy_delay_stub", "pc":f"0x{pc:08x}"})
            uc.reg_write(reg, value); uc.reg_write(UC_ARM_REG_PC, dest | 1); return
        event = None
        if pc == 0x080041C0:
            event = {"event":"uart_baud", "baud":uc.reg_read(UC_ARM_REG_R0)}
        elif pc == 0x080095A8:
            event = {"event":"i2c_bus_recovery_stub", "clocks":30}
        elif pc == 0x08009758:
            event = {"event":"pb13", "level":uc.reg_read(UC_ARM_REG_R0)}
        elif pc == 0x08016600:
            reg, ptr, words = (uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2))
            assert words == 1
            value = struct.unpack("<H", bytes(uc.mem_read(ptr, 2)))[0]
            event = {"event":"i2c_write", "address_7bit":22, "register":reg,
                     "value":value, "wire":bytes([0x2c,reg,value>>8,value&255]).hex(" ")}
        elif pc == 0x0800B268:
            ptr, n = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
            event = {"event":"hci_tx", "hex":bytes(uc.mem_read(ptr,n)).hex(" ")}
        elif pc == 0x08005FA8:
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), identity)
            event = {"event":"identity_stub", "synthetic":True}
        elif pc == 0x08018520:
            event = {"event":"delay_stub", "argument":uc.reg_read(UC_ARM_REG_R0)}
        if event is not None:
            events.append(event); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(0x08016641, 0x08002BF0, count=1000000)
    assert u.reg_read(UC_ARM_REG_PC) == 0x08002BF0
    assert [e["baud"] for e in events if e["event"] == "uart_baud"] == [115200,1500000]
    assert [e["hex"] for e in events if e["event"] == "hci_tx"][-2:] == [
        "01 02 fd 0a 80 01 60 00 00 80 60 e3 16 00", "01 02 fd 0a 80 01 40 00 00 80 00 b3 02 06"]
    return {"variant_ram_2000109a":variant, "stock_sha256":EXPECTED, "hardware_access":False,
            "rf_tail_recovered_from_scatterload":[{"register":reg,"value":value} for reg,value in struct.iter_unpack("<HH",rf_tail)],
            "events":events}

def main():
    out = ROOT / "build/research"; out.mkdir(parents=True,exist_ok=True)
    for variant in (0,1):
        result = recover(variant)
        (out / f"rda-init-variant-{variant}.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
        print(json.dumps({"variant":variant, "rf_tail":result["rf_tail_recovered_from_scatterload"],
                          "i2c_writes":sum(e["event"]=="i2c_write" for e in result["events"]),
                          "hci_packets":sum(e["event"]=="hci_tx" for e in result["events"]), "hardware_access":False}))

if __name__ == "__main__": main()
