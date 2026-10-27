"""Generate firmware/generated/rda_tables.h from the RDA SDK source and board tables."""
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RDA_SOURCE = ROOT / "vendor/rda5876/rdabt_r12_init.c"
BOARD_TABLES = ROOT / "vendor/rda5876/hci_initialization_tables.json"

def vendor_table_rows(name):
    """Select the SDK's ordinary UART, SSP-enabled, non-5990 R12 tables."""
    source = RDA_SOURCE.read_text()
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    body = source.split(f"{name}[][2]", 1)[1].split("};", 1)[0]
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("gcc", path="C:/msys64/ucrt64/bin")
    selected = subprocess.check_output(
        [compiler, "-E", "-P", "-x", "c", "-D__RDABT_ENABLE_SP__", "-"], input=body, text=True)
    return [(int(a, 16), int(v, 16)) for a, v in
            re.findall(r"\{\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*\}", selected)]

def vendor_patch_rows():
    return vendor_table_rows("rda_trap_12")

def generate():
    tables = json.loads(BOARD_TABLES.read_text())
    names = {"rf_i2c", "core_initial", "patches"}
    out = ["/* Board RF/clock/UART data: hci_initialization_tables.json.",
           " * ROM patches: RDA SDK rdabt_r12_init.c, rda_trap_12, unchanged",
           " * non-UART-break configuration. See vendor/rda5876/SOURCE.txt.",
           " * Stock-only runtime policy RAM writes are omitted; these are not",
           " * board RF calibration or RDA's documented PSKEY configuration. */",
           "#ifndef REDEV_RDA_TABLES_H", "#define REDEV_RDA_TABLES_H",
           "#include <stdint.h>", "typedef struct { uint32_t address, value; } rda_word_t;"]
    for table in tables:
        if table["name"] not in names:
            continue
        out.append(f"static const rda_word_t rda_{table['name']}[] = {{")
        if table["name"] == "patches":
            for address, value in vendor_patch_rows():
                out.append(f"    {{0x{address:08x}u, 0x{value:08x}u}},")
            out.append("};")
            continue
        for row in table["entries"]:
            address, value = int(row["target"], 0), int(row["value"], 0)
            out.append(f"    {{0x{address:08x}u, 0x{value:08x}u}},")
        out.append("};")
    out.append("static const rda_word_t rda_controller_config[] = {")
    # No external 32 kHz clock on the receiver. Preserve its known power floor.
    board_values = {0x80000074: 0x05025010, 0x800000a8: 0x0bbfbf30}
    for address, value in vendor_table_rows("rda_pskey_12"):
        value = board_values.get(address, value)
        out.append(f"    {{0x{address:08x}u, 0x{value:08x}u}},")
    out.append("};")
    # SDK's unsniff prerxon and ordinary (unpatched) H4 filter selection.
    out.extend(["static const uint8_t rda_vendor_setup[][9] = {",
                "    {0x01,0x72,0xfc,0x05,0x05,0x64,0x00,0x00,0x00},",
                "    {0x01,0x72,0xfc,0x05,0x01,0x00,0x00,0x00,0x00},", "};"])
    out.append("#endif\n")
    target = ROOT / "firmware/generated/rda_tables.h"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(out), encoding="utf-8")

if __name__ == "__main__":
    generate()
