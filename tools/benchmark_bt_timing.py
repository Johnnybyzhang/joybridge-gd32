"""Compare negotiated RDA Classic link timing, fresh BT reports and USB delivery.

Move the sticks continuously for an active-input comparison. Timing requests
are volatile and the original preference is restored, including on failure.
"""
from __future__ import annotations
from diagnostic_hid import receiver_devices
import argparse
import contextlib
import json
from pathlib import Path
import time

from benchmark_reports import live_result
from diagnostic_hid import exchange, TIMING_MODES


@contextlib.contextmanager
def receiver():
    import hid
    devices=receiver_devices()
    if len(devices)!=1:
        raise RuntimeError("Expected exactly one custom receiver")
    h=hid.device()
    h.open_path(devices[0]["path"])
    try:
        yield h
    finally:
        h.close()


def set_timing(mode: str) -> dict:
    with receiver() as h:
        exchange(h,0xfc,TIMING_MODES.index(mode))
        deadline=time.monotonic()+5
        last=None
        while time.monotonic()<deadline:
            last=exchange(h,0xfa,3)
            if not last["links"]:
                raise RuntimeError("No controller connected; press A on the saved Pro")
            if all(link["requested_timing"]==mode and link["state"] in ("done","failed")
                   for link in last["links"]):
                return last
            time.sleep(.05)
        raise RuntimeError(f"Timing did not settle: {last}")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--modes",nargs="+",choices=TIMING_MODES,default=TIMING_MODES[:7])
    parser.add_argument("--seconds",type=int,default=10)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    if args.seconds<=0: parser.error("--seconds must be positive")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    result={"phases":[],"restored":False}
    original=None
    try:
        with receiver() as h:
            result["ping"]=exchange(h,0xf0)
            if result["ping"]["firmware_version"]>=0x10019:
                raise RuntimeError("This historical probe was retired in firmware 0x10019")
            if result["ping"]["firmware_version"]<0x1000f:
                raise RuntimeError("This timing suite requires firmware 0x1000f or later")
            result["before"]=exchange(h,0xfa,3)
            if not result["before"]["links"]:
                raise RuntimeError("No controller connected; press A on the saved Pro")
            original=result["before"]["timing_preference"]
        for mode in args.modes:
            print(f"Negotiating {mode}...",flush=True)
            state=set_timing(mode)
            phase={"mode":mode,"negotiated":state}
            result["phases"].append(phase)
            if any(link["state"]=="failed" for link in state["links"]):
                print(f"Rejected/timeout: {json.dumps(state)}",flush=True)
            else:
                print(f"Measuring {mode} for {args.seconds}s; move sticks continuously.",flush=True)
                phase["measurement"]=live_result(args.seconds,"on")
            args.out.write_text(json.dumps(result,indent=2)+"\n")
    finally:
        if original is not None:
            try:
                result["after"]=set_timing(original)
                result["restored"]=all(x["state"]=="done" for x in result["after"]["links"])
            except Exception as exc:
                result["restore_error"]=str(exc)
        args.out.write_text(json.dumps(result,indent=2)+"\n")
    print(f"Saved {args.out}; original preference restored: {result['restored']}",flush=True)


if __name__=="__main__": main()
