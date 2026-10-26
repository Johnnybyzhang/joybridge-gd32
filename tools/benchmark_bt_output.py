"""Compare fresh Bluetooth input before/during/after on-device output cadence.

The MCU resends the current rumble payload and timestamps actual DMA submissions.
USB IMU is temporarily off in every phase to provide the existing 8 ms USB wake;
the physical controller continues sending full IMU reports. Each capture restores
the host's prior IMU request. This is a scheduling probe, not gyro-on acceptance.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path

from benchmark_bt_timing import receiver
from benchmark_reports import live_result
from diagnostic_hid import exchange


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds",type=int,default=10)
    parser.add_argument("--intervals-ms",type=int,nargs="+",default=[8])
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    if not 1<=args.seconds<=249:
        parser.error("--seconds plus 6 s startup margin must fit the protocol's one-byte duration")
    if any(not 1<=x<=255 for x in args.intervals_ms):
        parser.error("Intervals must fit the protocol's one-byte millisecond field")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    result={"phases":[],"probe_stopped":False}
    verified=False
    try:
        with receiver() as h:
            result["ping"]=exchange(h,0xf0)
            if result["ping"]["firmware_version"]>=0x10019:
                raise RuntimeError("This historical probe was retired in firmware 0x10019")
            if result["ping"]["firmware_version"]<0x10010:
                raise RuntimeError("This probe requires firmware 0x10010 or later")
            verified=True
            result["before"]=exchange(h,0xfa,3)
            if len(result["before"]["links"])!=1 or not result["before"]["links"][0]["ready"]:
                raise RuntimeError("Expected the saved Pro connected and ready")
            exchange(h,0xfd,0)
        for interval in [0,*args.intervals_ms,0]:
            phase={"output_interval_ms":interval}
            result["phases"].append(phase)
            if interval:
                with receiver() as h:
                    phase["request"]=exchange(h,0xfd,interval,args.seconds+6)
            print(f"Measuring {'passive' if not interval else str(interval)+' ms MCU output'} for {args.seconds}s.",flush=True)
            try:
                phase["measurement"]=live_result(args.seconds,"off")
            finally:
                with receiver() as h:
                    phase["probe_result"]=exchange(h,0xfa,4)
                    exchange(h,0xfd,0)
            args.out.write_text(json.dumps(result,indent=2)+"\n")
    finally:
        if verified:
            try:
                with receiver() as h:
                    exchange(h,0xfd,0)
                    result["after"]=exchange(h,0xfa,4)
                    result["probe_stopped"]=not result["after"]["output_probe"]["active"]
            except Exception as exc:
                result["stop_error"]=str(exc)
        args.out.write_text(json.dumps(result,indent=2)+"\n")
    print(f"Saved {args.out}; probe stopped: {result['probe_stopped']}",flush=True)


if __name__=="__main__": main()
