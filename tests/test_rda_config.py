"""Controller patches must never enable an uninitialized jump destination."""
import re
import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_every_enabled_rda_rom_patch_has_a_source_and_destination():
    path=ROOT / "firmware/generated/rda_tables.h"
    if not path.exists(): pytest.skip("Generate local radio tables first")
    source = path.read_text()
    body = source.split("rda_patches[] = {", 1)[1].split("};", 1)[0]
    written = {}
    for address, value in re.findall(r"\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}", body):
        address, value = int(address, 16), int(value, 16)
        if address == 0x40180000:
            for bit in range(16):
                if not value & (1 << bit):
                    continue
                # RDA's two banks: low eight at 04..20/24..40;
                # high eight at 100..11c/120..13c.
                trap = 0x40180004 + bit * 4 if bit < 8 else 0x40180100 + (bit - 8) * 4
                assert trap in written and trap + 0x20 in written, f"Uninitialized ROM patch slot {bit}"
                assert written[trap] != written[trap + 0x20]
        written[address] = value
    assert 0x40180000 in written, "Patch table must finish by enabling its initialized entries"
