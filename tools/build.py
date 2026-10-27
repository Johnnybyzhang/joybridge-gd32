"""Build host tests, ARM test fixtures and the JoyBridge firmware image."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
from check_image import validate
from generate_rda_tables import generate as generate_rda_tables

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CORE = sorted((ROOT / "firmware/src").glob("*.c"))
PLATFORM = ROOT / "firmware/platform/gd32f103"
BTSTACK = ROOT / "third_party/btstack/src"
CMSIS = ROOT / "third_party/CMSIS_6/CMSIS/Core/Include"
GD32_DFP = ROOT / "vendor/gd32/GD32F10x_DFP_2.0.3/Device"
USBD = ROOT / "vendor/gd32/GD32F10x_Firmware_Library_V2.7.0/Firmware/GD32F10x_usbd_library"
BT_SOURCES = ["btstack_linked_list", "btstack_memory", "btstack_memory_pool", "btstack_run_loop",
              "btstack_util", "ad_parser", "hci", "hci_cmd", "hci_dump", "l2cap", "l2cap_signaling",
              "classic/sdp_util"]
EXE = ".exe" if os.name == "nt" else ""


def find_tool(name: str) -> str:
    path = shutil.which(name) or shutil.which(name, path="C:/msys64/ucrt64/bin")
    if not path:
        raise SystemExit(f"{name} not found on PATH")
    return path


def arm_tool(name: str) -> str:
    return str(Path(find_tool("arm-none-eabi-gcc")).with_name(f"arm-none-eabi-{name}{EXE}"))


def run(args: list[str | Path]) -> None:
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def host_tests() -> None:
    cc = os.environ.get("CC") or find_tool("gcc")
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wstrict-prototypes", "-Ifirmware/include"]
    for name in ("core_tests", "nintendo_tests"):
        exe = BUILD / f"{name}{EXE}"
        run([cc, *common, "-O2", *CORE, f"tests/{name}.c", "-o", exe])
        run([exe])
    bt_objects = []
    for name in BT_SOURCES:
        source = ROOT / "tests/btstack_hci_fixture.c" if name == "hci" else BTSTACK / f"{name}.c"
        obj = BUILD / f"native_bt_{Path(name).name}.o"
        run([cc, "-std=c11", "-O2", f"-I{PLATFORM}", f"-I{BTSTACK}", "-c", source, "-o", obj])
        bt_objects.append(obj)
    exe = BUILD / f"bt_host_tests{EXE}"
    run([cc, *common, "-O2", f"-I{PLATFORM}", f"-I{BTSTACK}", "tests/bt_host_tests.c",
         "firmware/src/bridge.c", "firmware/src/nintendo.c", "firmware/src/pro_usb.c", *bt_objects, "-o", exe])
    run([exe])


def link(objects: list[Path], elf: Path, *extra: str) -> None:
    run([arm_tool("gcc"), "-mcpu=cortex-m3", "-mthumb", "-nostdlib", *objects, "-T", PLATFORM / "application.ld",
         "-Wl,--gc-sections", "-Wl,--build-id=none", *extra, "-lgcc", "-o", elf])
    run([arm_tool("objcopy"), "-O", "binary", "--gap-fill", "0xff", elf, elf.with_suffix(".bin")])


def firmware() -> None:
    generate_rda_tables()
    arm = arm_tool("gcc")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wstrict-prototypes", "-Ifirmware/include",
             "-mcpu=cortex-m3", "-mthumb", "-Os", "-g3", "-ffreestanding", "-fno-builtin",
             "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
             "-DGD32F10X_MD", f"-I{PLATFORM}", f"-I{GD32_DFP / 'Include'}", f"-I{GD32_DFP / 'Firmware/Peripherals/inc'}",
             f"-I{CMSIS}", f"-I{USBD / 'device/Include'}", f"-I{USBD / 'usbd/Include'}", f"-I{BTSTACK}"]
    vendor_flags = [f for f in flags if f not in ("-Werror", "-Wshadow", "-Wstrict-prototypes")]
    vendor_flags += ["-Wno-unused-parameter", "-Wno-unused-but-set-variable"]

    def compile_all(sources: list[Path], prefix: str = "", vendor: bool = False) -> list[Path]:
        objects = []
        for source in sources:
            obj = BUILD / f"{prefix}{source.stem}.o"
            run([arm, *(vendor_flags if vendor else flags), "-c", source, "-o", obj])
            objects.append(obj)
        return objects

    drivers = [GD32_DFP / f"Firmware/Peripherals/src/gd32f10x_{n}.c" for n in ("rcu", "gpio", "timer", "usart", "dma", "fmc")]
    usb_drivers = [USBD / f"device/Source/usbd_{n}.c" for n in ("core", "enum", "transc", "pwr")]
    base = compile_all([*CORE, PLATFORM / "board.c", PLATFORM / "hci_uart.c", PLATFORM / "sdk_usb.c",
                        PLATFORM / "freestanding.c", PLATFORM / "startup.S"])
    base += compile_all(drivers + usb_drivers, vendor=True)
    startup_fixture, = compile_all([ROOT / "tests/startup_fixture.c"])
    bt = compile_all([BTSTACK / f"{n}.c" for n in BT_SOURCES], prefix="bt_", vendor=True)
    physical = compile_all([PLATFORM / n for n in ("hardware.c", "diagnostic.c", "radio.c", "bt_host.c", "settings_flash.c")])
    physical += compile_all([USBD / "usbd/Source/usbd_lld_core.c", USBD / "usbd/Source/usbd_lld_int.c"], vendor=True)

    # Firmware image.
    link([*base, *physical, *bt], BUILD / "joybridge.elf", f"-Wl,-Map={BUILD / 'joybridge.map'}")
    run([arm_tool("size"), BUILD / "joybridge.elf"])
    print(json.dumps(validate((BUILD / "joybridge.bin").read_bytes()), indent=2))

    # Emulator fixtures used by the Python tests.
    link([*base, startup_fixture], BUILD / "startup-diagnostic.elf")
    board_functions = ["board_gpio_init", "board_button_pressed", "board_led_channels_set", "board_led_pwm_init",
                       "board_led_pwm_set", "hci_uart_init", "hci_uart_read", "hci_uart_send", "hci_uart_tx_idle",
                       "hci_uart_set_baud"]
    link([*base, startup_fixture], BUILD / "board-test.elf", *[f"-Wl,-u,{n}" for n in board_functions])
    link([*base, *compile_all([ROOT / "tests/sdk_usb_tests.c"])], BUILD / "sdk-usb-test.elf")
    radio_base = [o for o in base if o.name != "hci_uart.o"]
    link([*radio_base, BUILD / "radio.o", *compile_all([ROOT / "tests/radio_fixture.c"])], BUILD / "radio-test.elf")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-only", action="store_true", help="only build and run the native host tests")
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    host_tests()
    if not args.host_only:
        firmware()
