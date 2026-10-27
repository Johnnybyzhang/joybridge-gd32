"""Verify all supplied evidence checksums, without device access."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUNDLES = ["8bitdo_bootloader_reuse_analysis", "8bitdo_mcu_access_kit", "8bitdo_receiver_pinout_analysis"]

def verify() -> list[dict]:
    results = []
    for name in BUNDLES:
        base = (ROOT / "vendor/research" / name).resolve()
        count = 0
        for line in (base / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
            expected, relative = line.split(maxsplit=1)
            path = (base / relative.lstrip("*")).resolve()
            if not path.is_relative_to(base):
                raise ValueError("Evidence checksum path escapes its bundle")
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f"Evidence checksum mismatch: {name}/{relative}")
            count += 1
        results.append({"bundle": name, "files": count, "all_match": True})
    return results

if __name__ == "__main__":
    results = verify()
    out = ROOT / "build"; out.mkdir(exist_ok=True)
    (out / "input-validation.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))
