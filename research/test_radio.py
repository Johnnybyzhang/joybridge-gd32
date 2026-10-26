"""Focused UART/radio checks; no bootloader analysis or hardware access."""
import struct
import subprocess
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x08003400

def emulator(name):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000)
    u.mem_write(BASE, (ROOT / f"build/{name}.bin").read_bytes())
    u.mem_map(0x20000000, 0x5000); u.mem_map(0x40000000, 0x30000); u.mem_map(0xE000E000, 0x2000)
    symbols = subprocess.check_output(["C:/msys64/ucrt64/bin/arm-none-eabi-nm.exe", f"build/{name}.elf"], cwd=ROOT, text=True)
    addresses = {p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split()) == 3}
    return u, addresses

def test_uart_dma_ring_wrap_and_wire_drain():
    u, symbols = emulator("board-test")
    def read(address): return struct.unpack("<I", bytes(u.mem_read(address,4)))[0]
    def write(address,value): u.mem_write(address,struct.pack("<I",value))
    def call(name,r0=0,r1=0):
        for reg,value in [(UC_ARM_REG_R0,r0),(UC_ARM_REG_R1,r1),(UC_ARM_REG_SP,0x20004f00),(UC_ARM_REG_LR,0x08003001)]:
            u.reg_write(reg,value)
        u.emu_start(symbols[name]|1,0x08003000,count=100000)
        assert u.reg_read(UC_ARM_REG_PC) == 0x08003000
        return u.reg_read(UC_ARM_REG_R0)
    # 8 MHz HXTAL x6, AHB /1, APB1 /2 => 24 MHz USART input.
    write(0x40021004,0x00110008 | 0x400)
    assert call("hci_uart_init",115200) == 1
    assert read(0x40020074) == read(0x40020088) == 0x40004404
    assert read(0x40020070) == 2048
    assert read(0x4002006c)&0xff == 0xaf # enable, circular, increment, half/full/error IRQ
    assert read(0x40004414)&0x300 == 0 # no RTS/CTS
    assert read(0x40020080)&0xe == 0 # no TX IRQ
    ring = symbols["rx_buffer"]; destination = 0x20004700
    u.mem_write(ring,b"hello")
    write(0x40020070,2043)
    assert call("hci_uart_read",destination,5) == 5
    assert bytes(u.mem_read(destination,5)) == b"hello"
    # Full-wrap flag pending before its IRQ: snapshot includes this wrap once.
    write(0x40020000,2 << 20); write(0x40020070,2044)
    assert call("hci_uart_read",destination,64) == 64
    call("DMA0_Channel5_IRQHandler"); write(0x40020000,0)
    assert read(symbols["rx_wraps"]) == 1
    # Force an overrun: fail closed rather than deliver corrupted H4 bytes.
    write(0x40020000,2 << 20); write(0x40020070,2040)
    assert call("hci_uart_read",destination,64) == 0
    assert read(symbols["hci_uart_status"]+12) == 1
    assert call("hci_uart_init",115200) == 1
    write(0x40020000,0)
    u.mem_write(destination,bytes.fromhex("01 03 0c 00"))
    assert call("hci_uart_send",destination,4) == 1
    write(0x40020084,0); write(0x40004400,0x80) # DMA done; stop bit still pending
    assert call("hci_uart_tx_idle") == 0
    assert call("hci_uart_set_baud",1500000) == 0
    write(0x40004400,0xc0)
    assert call("hci_uart_tx_idle") == 1
    assert call("hci_uart_set_baud",1500000) == 1
    assert read(0x40004408) == 16

def test_radio_initialization_and_standard_hci_queries():
    u, symbols = emulator("radio-test")
    def stop(uc,address,size,context):
        if bytes(uc.mem_read(address,2)) == b"\x30\xbf": uc.emu_stop()
    u.hook_add(UC_HOOK_CODE,stop,begin=symbols["app_main"],end=symbols["app_main"]+256)
    raw = (ROOT / "build/radio-test.bin").read_bytes()
    u.reg_write(UC_ARM_REG_SP,0x20004fe8)
    u.emu_start(struct.unpack_from("<I",raw,4)[0],0x08000000,count=50000000)
    value = struct.unpack("<I",bytes(u.mem_read(symbols["radio_test_status"],4)))[0]
    assert value == 0x52444159, hex(value)
