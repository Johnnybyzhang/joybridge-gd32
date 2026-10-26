"""Measure fresh BT input with standard per-link ACL packet-mask restrictions.

Restores all locally supported packet types and the prior USB IMU request.
HCI's returned mask lists allowed types; it is not an over-the-air packet trace.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import time

from benchmark_bt_timing import receiver
from benchmark_reports import live_result
from diagnostic_hid import exchange, PACKET_MODES


def set_packets(mode):
    with receiver() as h:
        exchange(h,0xfe,PACKET_MODES.index(mode))
        deadline=time.monotonic()+4
        while time.monotonic()<deadline:
            state=exchange(h,0xfa,5)["packet_probe"]
            if state["state"] in ("done","failed"): return state
            time.sleep(.05)
        raise RuntimeError(f"Packet mask did not settle: {state}")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds",type=int,default=10)
    parser.add_argument("--modes",nargs="+",choices=PACKET_MODES,default=PACKET_MODES)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    if args.seconds<=0: parser.error("--seconds must be positive")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    result={"phases":[],"restored":False}
    verified=False
    try:
        with receiver() as h:
            result["ping"]=exchange(h,0xf0)
            if result["ping"]["firmware_version"]>=0x10019:
                raise RuntimeError("This historical probe was retired in firmware 0x10019")
            if result["ping"]["firmware_version"]<0x10011:
                raise RuntimeError("Packet mask probe requires firmware 0x10011 or later")
            verified=True
            result["before"]=exchange(h,0xfa,3)
            if len(result["before"]["links"])!=1 or not result["before"]["links"][0]["ready"]:
                raise RuntimeError("Expected one ready Pro")
            exchange(h,0xfd,0)
        for mode in args.modes:
            print(f"Negotiating {mode}...",flush=True)
            phase={"mode":mode,"negotiated":set_packets(mode)}
            result["phases"].append(phase)
            if phase["negotiated"]["mask_confirmed"]:
                phase["measurement"]=live_result(args.seconds,"on")
            else:
                print("Requested restriction was not confirmed; skipping rate attribution: "+json.dumps(phase),flush=True)
            args.out.write_text(json.dumps(result,indent=2)+"\n")
    finally:
        if verified:
            try:
                result["after"]=set_packets("all")
                result["restored"]=result["after"]["state"]=="done"
            except Exception as exc: result["restore_error"]=str(exc)
        args.out.write_text(json.dumps(result,indent=2)+"\n")
    print(f"Saved {args.out}; all local packet types restored: {result['restored']}",flush=True)


if __name__=="__main__": main()
