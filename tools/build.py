"""Build native checks and a loader-compatible ARM STARTUP DIAGNOSTIC."""
from __future__ import annotations
import argparse
import json
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
from check_image import validate
from sdk import installed_packs, firmware_library, GD32_VERSION, CMSIS_VERSION, FIRMWARE_VERSION

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CORE = sorted((ROOT / "firmware/src").glob("*.c"))

def find_tool(name: str, fallback: str) -> str:
    result = shutil.which(name)
    if result:
        return result
    path = Path(fallback)
    if path.is_file():
        return str(path)
    raise SystemExit(f"Missing {name}; install the toolchain or add it to PATH")

def run(args: list[str | Path]) -> None:
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)

def build(native_only: bool = False) -> None:
    BUILD.mkdir(exist_ok=True)
    cc = find_tool("gcc", "C:/msys64/ucrt64/bin/gcc.exe")
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wstrict-prototypes", "-Ifirmware/include"]
    native = BUILD / ("core_tests.exe" if os.name == "nt" else "core_tests")
    run([cc, *common, "-O2", *CORE, "tests/core_tests.c", "-o", native])
    run([native])
    nintendo_test = BUILD / ("nintendo_tests.exe" if os.name == "nt" else "nintendo_tests")
    run([cc, *common, "-O2", *CORE, "tests/nintendo_tests.c", "-o", nintendo_test])
    run([nintendo_test])
    host_test = BUILD / ("bt_host_tests.exe" if os.name == "nt" else "bt_host_tests")
    bt_names = ["btstack_linked_list", "btstack_memory", "btstack_memory_pool", "btstack_run_loop",
                "btstack_util", "ad_parser", "hci", "hci_cmd", "hci_dump", "l2cap", "l2cap_signaling",
                "classic/sdp_util"]
    native_bt = []
    for name in bt_names:
        obj = BUILD / f"native_bt_{Path(name).name}.o"
        run([cc, "-std=c11", "-O2",
             "-Ifirmware/platform/gd32f103", "-Ithird_party/btstack/src",
             "-c", (ROOT / "tests/btstack_hci_fixture.c" if name=="hci" else ROOT / "third_party/btstack/src" / f"{name}.c"), "-o", obj])
        native_bt.append(obj)
    run([cc, *common, "-O2",
         "-Ifirmware/platform/gd32f103", "-Ithird_party/btstack/src",
         "tests/bt_host_tests.c", "firmware/src/bridge.c", "firmware/src/nintendo.c",
         "firmware/src/pro_usb.c", *native_bt, "-o", host_test])
    run([host_test])
    if native_only:
        return
    if not (ROOT / "firmware/generated/rda_tables.h").exists():
        raise SystemExit("Generate local radio tables first; see docs/build.md. Vendor payloads are not distributed.")
    arm = find_tool("arm-none-eabi-gcc", "C:/msys64/ucrt64/bin/arm-none-eabi-gcc.exe")
    objcopy = str(Path(arm).with_name("arm-none-eabi-objcopy.exe" if os.name == "nt" else "arm-none-eabi-objcopy"))
    size = str(Path(arm).with_name("arm-none-eabi-size.exe" if os.name == "nt" else "arm-none-eabi-size"))
    flags = [*common, "-mcpu=cortex-m3", "-mthumb", "-Os", "-g3", "-ffreestanding", "-fno-builtin",
             "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]
    platform = ROOT / "firmware/platform/gd32f103"
    gd32, cmsis = installed_packs()
    library = firmware_library()
    usbd = library / "Firmware/GD32F10x_usbd_library"
    sdk_inc = gd32 / "Device/Firmware/Peripherals/inc"
    sdk_src = gd32 / "Device/Firmware/Peripherals/src"
    flags += ["-DGD32F10X_MD", f"-I{platform}", f"-I{gd32 / 'Device/Include'}",
              f"-I{sdk_inc}", f"-I{cmsis / 'CMSIS/Core/Include'}",
              f"-I{usbd / 'device/Include'}", f"-I{usbd / 'usbd/Include'}"]
    btstack = ROOT / "third_party/btstack/src"
    flags += [f"-I{btstack}"]
    bt_names = ["btstack_linked_list", "btstack_memory", "btstack_memory_pool", "btstack_run_loop",
                "btstack_util", "ad_parser", "hci", "hci_cmd", "hci_dump", "l2cap", "l2cap_signaling",
                "classic/sdp_util"]
    bt_objects = []
    for name in bt_names:
        source = btstack / f"{name}.c"
        obj = BUILD / f"bt_{source.stem}.o"
        run([arm, *[flag for flag in flags if flag not in ("-Werror", "-Wshadow", "-Wstrict-prototypes")],
             "-Wno-unused-parameter", "-c", source, "-o", obj])
        bt_objects.append(obj)
    drivers = [sdk_src / f"gd32f10x_{name}.c" for name in ("rcu", "gpio", "timer", "usart", "dma", "fmc")]
    usb_drivers = [usbd / f"device/Source/usbd_{name}.c" for name in ("core", "enum", "transc", "pwr")]
    sources = [*CORE, platform / "board.c", platform / "hci_uart.c", platform / "sdk_usb.c", platform / "freestanding.c", ROOT / "tests/startup_fixture.c", platform / "startup.S"]
    sources += drivers + usb_drivers
    objects = []
    for source in sources:
        obj = BUILD / f"{source.stem}.o"
        run([arm, *flags, "-c", source, "-o", obj]); objects.append(obj)
    elf = BUILD / "startup-diagnostic.elf"
    run([arm, "-mcpu=cortex-m3", "-mthumb", "-nostdlib", *objects, "-T", platform / "application.ld",
         "-Wl,--gc-sections", "-Wl,--build-id=none", f"-Wl,-Map={BUILD / 'startup-diagnostic.map'}", "-lgcc", "-o", elf])
    raw = BUILD / "startup-diagnostic.bin"
    run([objcopy, "-O", "binary", "--gap-fill", "0xff", elf, raw])
    result = validate(raw.read_bytes())
    (BUILD / "image-validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    run([size, elf])
    print(json.dumps(result, indent=2))
    # A second OFFLINE fixture retains board functions for register-level
    # emulation. The diagnostic's main still never calls these functions.
    board_elf = BUILD / "board-test.elf"
    board_functions = ["board_gpio_init", "board_button_pressed", "board_led_channels_set", "board_led_pwm_init", "board_led_pwm_set", "hci_uart_init", "hci_uart_read", "hci_uart_send", "hci_uart_tx_idle", "hci_uart_set_baud"]
    run([arm, "-mcpu=cortex-m3", "-mthumb", "-nostdlib", *objects, "-T", platform / "application.ld",
         "-Wl,--gc-sections", "-Wl,--build-id=none", *[f"-Wl,-u,{name}" for name in board_functions], "-lgcc", "-o", board_elf])
    run([objcopy, "-O", "binary", "--gap-fill", "0xff", board_elf, BUILD / "board-test.bin"])
    # An archive and a mock-hardware ARM fixture exercise the official USBD
    # core/enumeration/transaction layers; no SDK eval-board wiring is assumed.
    ar = str(Path(arm).with_name("arm-none-eabi-ar.exe" if os.name == "nt" else "arm-none-eabi-ar"))
    run([ar, "rcs", BUILD / "sdk-usbd.a", BUILD / "sdk_usb.o",
         *[BUILD / f"{source.stem}.o" for source in usb_drivers]])
    fixture = BUILD / "sdk_usb_tests.o"
    run([arm, *flags, "-c", ROOT / "tests/sdk_usb_tests.c", "-o", fixture])
    usb_elf = BUILD / "sdk-usb-test.elf"
    run([arm, "-mcpu=cortex-m3", "-mthumb", "-nostdlib",
         *[obj for obj in objects if obj.name != "startup_fixture.o"], fixture,
         "-T", platform / "application.ld", "-Wl,--gc-sections", "-Wl,--build-id=none",
         "-lgcc", "-o", usb_elf])
    run([objcopy, "-O", "binary", "--gap-fill", "0xff", usb_elf, BUILD / "sdk-usb-test.bin"])
    physical_sources = [platform / "hardware.c", platform / "diagnostic.c", platform / "radio.c", platform / "bt_host.c", platform / "settings_flash.c",
                        usbd / "usbd/Source/usbd_lld_core.c", usbd / "usbd/Source/usbd_lld_int.c"]
    physical_objects = []
    for source in physical_sources:
        obj = BUILD / f"{source.stem}.o"
        vendor_flags = ["-Wno-unused-parameter", "-Wno-unused-but-set-variable"] if source in physical_sources[5:] else []
        run([arm, *flags, *vendor_flags, "-c", source, "-o", obj])
        physical_objects.append(obj)
    hid_elf = BUILD / "hid-diagnostic.elf"
    run([arm, "-mcpu=cortex-m3", "-mthumb", "-nostdlib",
         *[obj for obj in objects if obj.name != "startup_fixture.o"], *physical_objects, *bt_objects,
         "-T", platform / "application.ld", "-Wl,--gc-sections", "-Wl,--build-id=none",
         f"-Wl,-Map={BUILD / 'hid-diagnostic.map'}", "-lgcc", "-o", hid_elf])
    hid_raw = BUILD / "hid-diagnostic.bin"
    run([objcopy, "-O", "binary", "--gap-fill", "0xff", hid_elf, hid_raw])
    hid_manifest = {**validate(hid_raw.read_bytes()), "kind": "hid-diagnostic",
                    "version": int(re.search(r"^#define DIAGNOSTIC_VERSION (0x[0-9a-fA-F]+)u$",
                                             (ROOT / "firmware/include/diagnostic_hid.h").read_text(),re.M)[1],16),
                    "usb_vid": "057e", "usb_pid": "20ff",
                    "usb_identities": {"idle": "20ff", "pro": "2009", "joycons": "200e"},
                    "bootloader_return": "guarded HID command; stock power-on button path"}
    (BUILD / "hid-diagnostic.json").write_text(json.dumps(hid_manifest, indent=2) + "\n", encoding="utf-8")
    run([size, hid_elf])
    radio_fixture = BUILD / "radio_fixture.o"
    run([arm, *flags, "-c", ROOT / "tests/radio_fixture.c", "-o", radio_fixture])
    radio_elf = BUILD / "radio-test.elf"
    run([arm, "-mcpu=cortex-m3", "-mthumb", "-nostdlib",
         *[obj for obj in objects if obj.name not in ("startup_fixture.o", "hci_uart.o")],
         BUILD / "radio.o", radio_fixture, "-T", platform / "application.ld",
         "-Wl,--gc-sections", "-Wl,--build-id=none", "-lgcc", "-o", radio_elf])
    run([objcopy, "-O", "binary", "--gap-fill", "0xff", radio_elf, BUILD / "radio-test.bin"])
    provenance = {"gd32_dfp": GD32_VERSION, "arm_cmsis": CMSIS_VERSION,
                  "gd32_firmware_library": FIRMWARE_VERSION, "vendor_sources": []}
    for source in drivers + usb_drivers + physical_sources[5:]:
        provenance["vendor_sources"].append({"path": str(source),
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
    (BUILD / "sdk-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-only", action="store_true")
    build(parser.parse_args().native_only)
