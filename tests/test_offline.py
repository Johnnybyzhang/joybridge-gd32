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

def flash_backup():
    return (ROOT / "vendor/research/8bitdo_bootloader_reuse_analysis/full_backup/backup-01/flash_backup.bin").read_bytes()

def test_original_loader_handoff_and_new_startup(image):
    flash = bytearray(flash_backup())
    flash[APP_BASE - 0x08000000:APP_BASE - 0x08000000 + len(image)] = image
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(0x08000000, bytes(flash))
    u.mem_map(0x20000000, 0x5000); u.mem_write(0x20000000, b"\xa5" * 0x5000)
    u.mem_map(0xE000E000, 0x2000)
    u.mem_write(0xE000E010, struct.pack("<III", 7, 480000, 123))
    u.mem_write(0x20004FF0, struct.pack("<I", 0x5A5AA5A5))
    u.reg_write(UC_ARM_REG_R0, APP_BASE); u.reg_write(UC_ARM_REG_SP, 0x20003FB8)
    reset = struct.unpack_from("<I", image, 4)[0] & ~1
    u.emu_start(0x0800137D, reset, count=100)
    assert u.reg_read(UC_ARM_REG_PC) == reset
    assert u.reg_read(UC_ARM_REG_SP) == 0x20004FE8  # original loader's post-MSR POP
    writes = []
    def write_hook(uc, access, address, size, value, context):
        writes.append((address, size, value))
    u.hook_add(UC_HOOK_MEM_WRITE, write_hook)
    # Stop on WFI after application has run. No peripheral/interrupt emulation.
    def code_hook(uc, address, size, context):
        if bytes(uc.mem_read(address, 2)) == b"\x30\xbf":
            uc.emu_stop()
    u.hook_add(UC_HOOK_CODE, code_hook)
    u.emu_start(reset | 1, 0x08000000, count=100000)
    assert u.reg_read(UC_ARM_REG_PRIMASK) == 1
    assert struct.unpack("<I", bytes(u.mem_read(0xE000ED08, 4)))[0] == APP_BASE
    assert bytes(u.mem_read(0x20004FF0, 4)) == b"\0" * 4
    assert bytes(u.mem_read(0xE000E010, 12)) == b"\0" * 12
    assert len([a for a, _, v in writes if 0xE000E180 <= a < 0xE000E1A0 and v == 0xFFFFFFFF]) == 8
    assert len([a for a, _, v in writes if 0xE000E280 <= a < 0xE000E2A0 and v == 0xFFFFFFFF]) == 8
    assert not any(0x08000000 <= a < 0x08020000 or 0x40000000 <= a < 0x60000000 for a, _, _ in writes)
    symbols = subprocess.check_output([NM, "build/startup-diagnostic.elf"], cwd=ROOT, text=True)
    addresses = {parts[2]: int(parts[0], 16) for line in symbols.splitlines() if len(parts := line.split()) == 3}
    assert bytes(u.mem_read(addresses["diagnostic_status"], 4)) == struct.pack("<I", 0x52454459)
    assert bytes(u.mem_read(addresses["diagnostic_report"], 1)) == b"\x30"

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

def test_packaged_image_against_original_decoder(image):
    dat, result = package(image, 0x10000)
    payload = dat[28:]
    expected = image + b"\xff" * (result["padded_bytes"] - len(image))
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(0x08000000, flash_backup())
    u.mem_map(0x20000000, 0x5000)
    def call(address, r0=0, r1=0, r2=0):
        for reg, val in [(UC_ARM_REG_R0, r0), (UC_ARM_REG_R1, r1), (UC_ARM_REG_R2, r2),
                         (UC_ARM_REG_R3, 0), (UC_ARM_REG_SP, 0x20004000), (UC_ARM_REG_LR, 0x08002BF1)]:
            u.reg_write(reg, val)
        u.emu_start(address | 1, 0x08002BF0, count=100000)
        assert u.reg_read(UC_ARM_REG_PC) == 0x08002BF0
    call(0x0800090C)
    for offset in range(0, len(payload), 512):
        u.mem_write(0x20000178, payload[offset:offset+512])
        call(0x080026C4, 0x20000178, offset, 512)
        assert bytes(u.mem_read(0x20000178, 512)) == expected[offset:offset+512]

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


def test_live_diagnostic_vectors_and_bootloader_return(image):
    raw = (ROOT / "build/joybridge.bin").read_bytes()
    validate(raw)
    symbols = subprocess.check_output([NM, "build/joybridge.elf"], cwd=ROOT, text=True)
    addresses = {p[2]:int(p[0],16) for line in symbols.splitlines() if len(p:=line.split())==3}
    for vector, name in [(15,"SysTick_Handler"),(35,"USBD_HP_CAN0_TX_IRQHandler"),(36,"USBD_LP_CAN0_RX0_IRQHandler")]:
        assert struct.unpack_from("<I", raw, vector*4)[0] == addresses[name] | 1
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    u.mem_map(0x08000000, 0x20000); u.mem_write(0x08000000, flash_backup()); u.mem_write(APP_BASE, raw)
    u.mem_map(0x20000000, 0x5000); u.mem_map(0x40000000, 0x30000); u.mem_map(0xE000E000, 0x2000)
    writes = []
    def watch_reset(uc, access, address, size, value, context):
        writes.append((address, value))
        if address == 0xE000ED0C:
            uc.emu_stop()
    hook = u.hook_add(UC_HOOK_MEM_WRITE, watch_reset)
    u.reg_write(UC_ARM_REG_SP, 0x20004000)
    u.emu_start(addresses["diagnostic_enter_bootloader"] | 1, 0x08000000, count=3000000)
    assert (0x20004ff0,0x5a5aa5a5) in writes
    assert any(a == 0xE000ED0C and value & 4 for a, value in writes)
    assert not any(0x08000000 <= a < 0x08020000 for a, _ in writes)
    u.hook_del(hook)
    u.reg_write(UC_ARM_REG_SP, 0x20004000); u.reg_write(UC_ARM_REG_LR, 0x08002ff1)
    u.emu_start(0x0800133d, 0x08002ff0, count=10000)
    assert u.reg_read(UC_ARM_REG_PC) == 0x08002ff0
    assert u.reg_read(UC_ARM_REG_R0) == 1  # ORIGINAL loader selects USB mode

def test_live_diagnostic_update_through_original_loader(image):
    from bootloader import update_packet as packet, check_ack, installed_bytes
    raw = (ROOT / "build/joybridge.bin").read_bytes()
    dat, _ = package(raw, 0x10001)
    spec = importlib.util.spec_from_file_location("stock_update", ROOT / "vendor/research/8bitdo_bootloader_reuse_analysis/bootloader_analysis/update/emulate_update_handler.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    model = module.Model()
    tag = flash_backup()[0x341c:0x3428]
    assert packet(1,dat[:28])[:8] == bytes.fromhex("23 00 16 1f 00 01 1c 00")
    result = model.call_update(1, version=0x10001, length=len(dat)-28, reserved=dat[12:28])
    assert result["return_u32"] == "0x00000000"
    for offset in range(28,len(dat),32):
        chunk = dat[offset:offset+32]
        wire = packet(0,chunk)
        assert wire[:8] == bytes.fromhex("27 00 16 23 00 00 20 00")
        assert wire[8:40] == chunk
        result = model.call_update(0,chunk)
        assert result["return_u32"] == "0x00000000"
    assert model.call_update(6,tag)["return_u32"] == "0x00000000"
    assert bytes(model.uc.mem_read(APP_BASE,len(dat)-28)) == installed_bytes(raw,0x10001,tag)
    assert model.call_update(2)["usb_exit_flag"] == 1
    assert bytes(model.uc.mem_read(0x08000000,0x3400)) == flash_backup()[:0x3400]
    assert bytes(model.uc.mem_read(0x0801ec00,0x800)) == flash_backup()[0x1ec00:0x1f400]
    import json
    capture = json.loads((ROOT / "vendor/research/8bitdo_mcu_access_kit/evidence/selected_capture_packets.json").read_text())
    for frame, tag_reply in [(1347,False),(1351,False),(13833,True)]:
        check_ack(bytes.fromhex(next(row["wire_hex"] for row in capture if row["frame"] == frame)), 6 if tag_reply else 0)

@pytest.mark.parametrize("variant",[0,1])
def test_recovered_rda_initialization_trace(variant):
    from recover_rda import recover
    result = recover(variant)
    assert result["rf_tail_recovered_from_scatterload"]==[{"register":48,"value":297},{"register":48,"value":299}]
    assert sum(e["event"]=="i2c_write" for e in result["events"])==96
    assert sum(e["event"]=="hci_tx" for e in result["events"])==76
    import json
    tables = {t["name"]:t["entries"] for t in json.loads((ROOT / "vendor/research/8bitdo_receiver_pinout_analysis/evidence/hci_initialization_tables.json").read_text())}
    branch = "conditional_nonzero" if variant else "conditional_zero"
    rows = [*tables["core_initial"], *tables["patches"], *tables[branch], *tables[branch+"_tail"]]
    packets = [bytes.fromhex(e["hex"]) for e in result["events"] if e["event"]=="hci_tx"]
    for packet,row in zip(packets[:71], rows, strict=True):
        assert packet==bytes.fromhex("01 02 fd 0a 00 01")+struct.pack("<II",int(row["target"],0),int(row["value"],0))
    rf = [(e["register"],e["value"]) for e in result["events"] if e["event"]=="i2c_write"]
    expected_rf = [(int(row["target"],0),int(row["value"],0)) for row in tables["rf_i2c"]]
    assert rf==expected_rf+expected_rf+[(0x30,0x129),(0x30,0x12b)]
