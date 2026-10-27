from __future__ import annotations
import importlib.util
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import pytest
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK

ROOT = Path(__file__).resolve().parents[1]
NM = shutil.which("arm-none-eabi-nm")
sys.path.insert(0, str(ROOT / "tools"))
from check_image import APP_BASE, validate
from package_image import package

@pytest.fixture(scope="session")
def image():
    return (ROOT / "build/startup-diagnostic.bin").read_bytes()

@pytest.mark.parametrize("offset,value", [(0, 0x20005000), (0, 0x20004FE1), (4, 0x08003570),
    (4, 0x08000001), (8, 0), (12, 0x08000001), (0x1C, 0), (0x20, 0), (0x24, 0), (0x34, 0)])
def test_reject_invalid_image(image, offset, value):
    bad = bytearray(image); struct.pack_into("<I", bad, offset, value)
    with pytest.raises(ValueError): validate(bytes(bad))

def test_reject_oversized_and_unsafe_entry(image):
    with pytest.raises(ValueError): validate(image + b"\xff" * 0x1B800)
    bad = bytearray(image); reset = struct.unpack_from("<I", image, 4)[0] & ~1
    bad[reset-APP_BASE:reset-APP_BASE+2] = b"\0\0"
    with pytest.raises(ValueError): validate(bytes(bad))

def test_board_gpio_and_pwm_without_interrupts(image):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000,0x20000)
    u.mem_write(APP_BASE,(ROOT / "build/board-test.bin").read_bytes())
    u.mem_map(0x20000000,0x5000)
    u.mem_map(0x40000000,0x30000)
    symbols = subprocess.check_output([NM, "build/board-test.elf"], cwd=ROOT,text=True)
    addresses = {p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split())==3}
    def read(address): return struct.unpack("<I",bytes(u.mem_read(address,4)))[0]
    def write(address,value): u.mem_write(address,struct.pack("<I",value))
    def call(name,r0=0,r1=0):
        for reg,value in [(UC_ARM_REG_R0,r0),(UC_ARM_REG_R1,r1),(UC_ARM_REG_SP,0x20004000),(UC_ARM_REG_LR,0x08003001)]: u.reg_write(reg,value)
        u.emu_start(addresses[name]|1,0x08003000,count=10000)
        assert u.reg_read(UC_ARM_REG_PC)==0x08003000
        return u.reg_read(UC_ARM_REG_R0)
    write(0x40010004,0x02000000) # existing SWD-only setting
    write(0x40010800,0xaaaaaaaa); write(0x40010c00,0xaaaaaaaa); write(0x40010c04,0xaaaaaaaa)
    call("board_gpio_init")
    assert read(0x40010800)==0xaaaaaa8a
    assert read(0x40010c00)==0xaa1aaaaa
    assert read(0x40010c04)==0xaaaaaa11
    for channel,pin in [(1,0x200),(2,0x100),(4,0x20),(7,0x320),(0,0)]:
        call("board_led_channels_set",channel)
        assert read(0x40010c10)==((0x320&~pin)|(pin<<16))
    write(0x40010808,2); assert call("board_button_pressed")==0
    write(0x40010808,0); assert call("board_button_pressed")==1
    assert call("board_led_pwm_init",48000000,7)==1
    assert read(0x40010004)&0x07000000==0x02000000
    assert read(0x4000040c)==read(0x4000080c)==0 # no timer interrupts/DMA requests
    assert read(0x4000042c)==read(0x4000082c)==23999
    call("board_led_pwm_set",16)
    assert read(0x40000438)==read(0x4000083c)==read(0x40000840)==24000*16//255
    assert call("board_led_pwm_init",48000000,8)==0
    # One UART idle interrupt per DMA burst, no byte RX interrupts. The cycle
    # counter timestamps burst spacing independently of the 10 ms service tick.
    u.mem_map(0xe0001000,0x1000); u.mem_map(0xe000e000,0x2000)
    assert call("hci_uart_init",1500000)==1
    assert read(0x4000440c)&(1<<4) and not read(0x4000440c)&(1<<5)
    assert read(0xe0001000)&1 and read(0xe000e104)&(1<<6)
    status=addresses["hci_uart_status"]
    write(0x40004400,1<<4); write(0xe0001004,100000)
    call("USART1_IRQHandler")
    assert read(status+24)==1 and read(status+28)==0
    write(0xe0001004,580000); call("USART1_IRQHandler")
    assert read(status+24)==2 and read(status+28)==1
    assert read(status+32)==10000 and read(status+36)==10000
    write(0x40004400,(1<<4)|(1<<2)); call("USART1_IRQHandler")
    assert read(status+16)==1 and bytes(u.mem_read(status+44,1))==b"\x01"

def test_official_sdk_usb_enumeration_and_backpressure(image):
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

def test_rda_sdk_patch_bringup(image):
    raw = (ROOT / "build/radio-test.bin").read_bytes()
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(APP_BASE, raw)
    u.mem_map(0x20000000, 0x5000)
    u.mem_map(0x40000000, 0x30000)
    u.mem_map(0xE000E000, 0x2000)
    def stop_on_wfi(uc, address, size, context):
        if bytes(uc.mem_read(address, 2)) == b"\x30\xbf":
            uc.emu_stop()
    u.hook_add(UC_HOOK_CODE, stop_on_wfi)
    u.reg_write(UC_ARM_REG_SP, 0x20004FE8)
    u.emu_start(struct.unpack_from("<I", raw, 4)[0], 0x08000000, count=10000000)
    symbols = subprocess.check_output([NM, "build/radio-test.elf"], cwd=ROOT, text=True)
    addresses = {p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split())==3}
    status = struct.unpack("<I", bytes(u.mem_read(addresses["radio_test_status"], 4)))[0]
    assert status == 0x52444159, f"RDA radio fixture failed: {status:08x}"


def test_diagnostic_clock_without_extra_tick_interrupts(image):
    raw=(ROOT/"build/joybridge.bin").read_bytes()
    symbols=subprocess.check_output([NM,"build/joybridge.elf"],cwd=ROOT,text=True)
    addresses={p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split())==3}
    u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
    u.mem_map(0x08000000,0x20000); u.mem_write(APP_BASE,raw)
    u.mem_map(0x20000000,0x5000); u.mem_map(0xe000e000,0x2000)
    u.mem_write(0xe000e014,struct.pack("<I",479999))
    for base,remaining,expected in [(1000,479999,1000),(1000,95999,1008),(1000,0,1009),(1010,479999,1010)]:
        u.mem_write(addresses["milliseconds"],struct.pack("<I",base))
        u.mem_write(0xe000e018,struct.pack("<I",remaining))
        u.reg_write(UC_ARM_REG_SP,0x20004000); u.reg_write(UC_ARM_REG_LR,0x08003001)
        u.emu_start(addresses["diagnostic_millis"]|1,0x08003000,count=1000)
        assert u.reg_read(UC_ARM_REG_PC)==0x08003000
        assert u.reg_read(UC_ARM_REG_R0)==expected


