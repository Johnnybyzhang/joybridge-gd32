"""Run receiver acceptance checks and save one timestamped evidence folder."""
from __future__ import annotations
import argparse
from datetime import datetime
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test",choices=["all","rate","motion","rumble","snapshot"],default="all")
    parser.add_argument("--seconds",type=int,choices=range(5,121),default=30,help="Seconds per rate phase")
    parser.add_argument("--out",type=Path,help="New evidence directory (default: timestamp under build/hardware)")
    args=parser.parse_args()
    out=args.out or ROOT/"build/hardware"/datetime.now().strftime("acceptance-%Y%m%d-%H%M%S")
    out=out.resolve(); out.mkdir(parents=True,exist_ok=False)
    def run(name,arguments,filename):
        result=subprocess.run([sys.executable,str(ROOT/"tools"/name),*arguments,"--out",str(out/filename)],
                              cwd=ROOT,text=True,capture_output=True)
        if result.returncode:
            (out/(filename+".error.txt")).write_text(result.stdout+result.stderr,encoding="utf-8")
            raise RuntimeError(f"{name} failed: {result.stderr.strip()}")
        return json.loads((out/filename).read_text())
    def snapshot(filename):
        return run("diagnostic_hid.py",["--host-status","--radio-status","--output-status","--rate-status","--trace"],filename)
    print(f"Evidence: {out}",flush=True)
    before=snapshot("before.json")
    print(f"Firmware: 0x{before['ping']['firmware_version']:x}; connected: {before['host']['connected']}; setup: {before['host']['setup_stage']}",flush=True)
    if args.test!="snapshot" and (before["host"]["connected"]!=1 or before["host"]["setup_stage"]!=10):
        raise SystemExit("Select blue profile 1, press A on the saved Pro, then rerun after it connects.")
    results={}
    try:
        if args.test in ("all","rate"):
            print(f"Rate: {args.seconds}s gyro off, then {args.seconds}s gyro on. Move both sticks continuously throughout both phases.",flush=True)
            rate=run("benchmark_reports.py",["--seconds",str(args.seconds)],"rate.json")
            results["rate"]=[{"imu":x["imu_enabled"],"usb_hz":x["rate_hz"],"bt_hz":x.get("bt_raw_hid_hz",x["bt_publication_hz"]),
                              "p99_ms":x["interval_ms"]["p99"],"max_ms":x["interval_ms"]["max"]} for x in rate["phases"]]
            print(json.dumps(results["rate"],indent=2),flush=True)
        if args.test in ("all","motion"):
            print("Motion starts in 3s: move both sticks fully, press A/B and stick clicks, then tilt for 20s.",flush=True)
            time.sleep(3)
            results["motion"]=run("pro_usb_probe.py",["--seconds","20"],"motion.json")
            print("Motion capture saved.",flush=True)
        if args.test in ("all","rumble"):
            print("Rumble starts in 3s: hold the Pro and expect one second of vibration followed by a stop.",flush=True)
            time.sleep(3)
            results["rumble_api"]=run("sdl_rumble_probe.py",[],"rumble.json")
            results["rumble_physical"]="Record observation in observations.md; API success alone is not physical acceptance."
        if args.test=="snapshot": print("Snapshot saved.",flush=True)
    finally:
        try: snapshot("after.json")
        finally:
            (out/"summary.json").write_text(json.dumps(results,indent=2)+"\n")
            (out/"observations.md").write_text(
                "# Physical observations\n\n"
                "- [ ] One-second SDL rumble felt and stopped\n"
                "- [ ] Both sticks/buttons and tilt captured\n"
                "- [ ] Double tap green, double tap blue, A reconnects with dim blue\n"
                "- [ ] Hold 2s then single tap cancels pairing; A reconnects\n"
                "- [ ] Actual Steam Input game rumble felt and stopped\n\n"
                "Game and Steam Input setting:\n\nOther observations:\n",encoding="utf-8")
    print(f"Done. Results and physical checklist: {out}",flush=True)


if __name__=="__main__": main()
