"""Measure the real Pro directly over Windows Bluetooth, with live progress.

Requests full report 0x30 and gyro, matching the receiver's physical BT setup.
Use --passive to measure the existing 0x30 or 0x3f mode without sending commands.
Keep moving both sticks during the run.
All full reports are counted separately from changed input and unique timers.
"""
from __future__ import annotations
import argparse
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import time
import hid
from benchmark_reports import timing


def sticks(report):
    if report[0]==0x3f:
        return [int.from_bytes(report[i:i+2],"little") for i in (4,6,8,10)]
    values=[]
    for offset in (6,9):
        values.extend((report[offset]|((report[offset+1]&15)<<8),
                       (report[offset+1]>>4)|(report[offset+2]<<4)))
    return values


def initialize_full_mode(handle, timeout=3):
    """Use Bluetooth 0x01 subcommands, never the USB 0x80 handshake."""
    counter=0
    acknowledgements=[]
    for command,payload in ((0x40,b"\x01"),(0x03,b"\x30")):
        report=bytearray(49); report[0]=1; report[1]=counter&15; counter+=1
        report[2:10]=bytes.fromhex("00 01 40 40 00 01 40 40")
        report[10]=command; report[11:11+len(payload)]=payload
        if handle.write(report)!=len(report): raise RuntimeError(f"Incomplete Bluetooth subcommand 0x{command:02x}")
        deadline=time.monotonic()+timeout; seen=Counter()
        while time.monotonic()<deadline:
            reply=bytes(handle.read(128,100))
            if not reply: continue
            seen[f"{reply[0]:02x}"]+=1
            if len(reply)>=15 and reply[0]==0x21 and reply[14]==command:
                if not reply[13]&0x80: raise RuntimeError(f"Pro NACK for subcommand 0x{command:02x}")
                acknowledgements.append({"command":f"{command:02x}","ack":f"{reply[13]:02x}"})
                print(f"ACK: command 0x{command:02X} -> 0x{reply[13]:02X}",flush=True)
                break
        else: raise RuntimeError(f"No ACK for subcommand 0x{command:02x}; received report IDs: {dict(seen)}")
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        report=bytes(handle.read(128,100))
        if len(report)>=49 and report[0]==0x30: return acknowledgements
    raise RuntimeError("Pro acknowledged full mode but sent no complete 0x30 report")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds",type=int,default=30)
    parser.add_argument("--countdown",type=int,default=5)
    parser.add_argument("--out",type=Path)
    parser.add_argument("--passive",action="store_true",help="Do not initialize; measure the current 0x30 or simple 0x3F report stream")
    args=parser.parse_args()
    if not 5<=args.seconds<=120 or not 0<=args.countdown<=15:
        parser.error("Use 5..120 seconds and 0..15 countdown seconds")
    targets=[d for d in hid.enumerate(0x057e,0x2009) if int(d.get("bus_type",0))==2
             and d.get("product_string")!="8BitDo HID Diagnostic"]
    if len(targets)!=1: raise RuntimeError(f"Expected one direct Bluetooth Pro, found {len(targets)}")
    target=targets[0]
    output=args.out or Path("build/hardware")/datetime.now().strftime("direct-bt-rate-%Y%m%d-%H%M%S.json")
    result={"clock":"Windows HID read completion","transport":"direct PC Bluetooth HID",
            "device":{k:target.get(k) for k in ("product_string","serial_number","bus_type")},
            "passive":args.passive,"windows":[],"requested_seconds":args.seconds}
    print(f"Direct Bluetooth Pro: {target['product_string']} ({target.get('serial_number','')})",flush=True)
    h=hid.device(); h.open_path(target["path"])
    stamps=[]; timer_values=[]; deltas=Counter(); other=Counter()
    report_counts=Counter(); selected_id=None
    changes=0; motion=0; low=[4095]*4; high=[0]*4
    prior_state=None; prior_sticks=None; prior_timer=None
    start=time.perf_counter(); interval_start=start; next_print=start+1
    window_start_count=0; window_changes=0; window_motion=0; window_fresh=0
    try:
        h.set_nonblocking(0)
        if args.passive:
            print("Passive capture: measuring the current report mode.",flush=True)
        else:
            print("Initializing Bluetooth full mode 0x30 with IMU enabled; rumble payload stays neutral.",flush=True)
            result["initialization"]=initialize_full_mode(h)
            selected_id=0x30
        print("MOVE BOTH STICKS CONTINUOUSLY during the measurement.",flush=True)
        for remaining in range(args.countdown,0,-1):
            print(f"Starting in {remaining}...",flush=True)
            deadline=time.monotonic()+1
            while time.monotonic()<deadline: h.read(128,100)
        h.set_nonblocking(1)
        # Bound draining if a host produces data faster than this process reads.
        deadline=time.monotonic()+.25
        while time.monotonic()<deadline and h.read(128): pass
        h.set_nonblocking(0)
        start=time.perf_counter(); interval_start=start; next_print=start+1
        print("MEASUREMENT STARTED",flush=True)
        while time.perf_counter()-start<args.seconds:
            report=bytes(h.read(128,100)); now=time.perf_counter()
            if report: report_counts[f"{report[0]:02x}"]+=1
            complete=bool(report) and ((report[0]==0x30 and len(report)>=49) or (report[0]==0x3f and len(report)>=12))
            if complete and selected_id is None:
                selected_id=report[0]; print(f"Detected input mode: 0x{selected_id:02X}",flush=True)
                if selected_id==0x3f: low=[65535]*4
            if complete and report[0]==selected_id:
                stamps.append(now)
                if selected_id==0x30: timer_values.append(report[1])
                current_state=report[3:12] if selected_id==0x30 else report[1:12]
                current_sticks=sticks(report)
                if prior_state is not None and current_state!=prior_state: changes+=1; window_changes+=1
                threshold=32 if selected_id==0x30 else 512
                if prior_sticks is not None and max(abs(a-b) for a,b in zip(current_sticks,prior_sticks))>=threshold:
                    motion+=1; window_motion+=1
                if selected_id==0x30 and prior_timer is not None:
                    delta=(report[1]-prior_timer)&255; deltas[delta]+=1
                    if delta: window_fresh+=1
                for i,value in enumerate(current_sticks): low[i]=min(low[i],value); high[i]=max(high[i],value)
                prior_state=current_state; prior_sticks=current_sticks
                if selected_id==0x30: prior_timer=report[1]
            elif report: other[f"{report[0]:02x}"]+=1
            if now>=next_print or now-start>=args.seconds:
                elapsed=now-interval_start; count=len(stamps)-window_start_count
                row={"end_seconds":now-start,"duration":elapsed,"reports":count,"rate_hz":count/elapsed,
                     "fresh_timers":window_fresh if selected_id==0x30 else None,
                     "changed_inputs":window_changes,"stick_motion_reports":window_motion}
                result["windows"].append(row)
                fresh=f"{window_fresh/elapsed:6.1f}/s" if selected_id==0x30 else "   n/a  "
                print(f"[{min(round(now-start),args.seconds):02d}/{args.seconds}s] reports={row['rate_hz']:6.1f} Hz  "
                      f"fresh={fresh}  changes={window_changes:3d}  stick-motion={window_motion:3d}  IDs={dict(report_counts)}",flush=True)
                interval_start=now; next_print=now+1; window_start_count=len(stamps)
                window_changes=window_motion=window_fresh=0
    except KeyboardInterrupt:
        result["interrupted"]=True
    except Exception as exc:
        result["error"]=str(exc)
        raise
    finally:
        h.close()
        result.update(elapsed_seconds=time.perf_counter()-start,changed_inputs=changes,stick_motion_reports=motion,
                      sticks_min=low,sticks_max=high,timer_delta_histogram=dict(sorted(deltas.items())),other_reports=dict(other))
        result["report_counts"]=dict(report_counts)
        result["input_report_id"]=f"{selected_id:02x}" if selected_id is not None else None
        if len(stamps)>=2: result.update(timing(stamps))
        result["report_timestamps_seconds"]=[stamp-start for stamp in stamps]
        result["report_timers"]=timer_values
        output.parent.mkdir(parents=True,exist_ok=True)
        output.write_text(json.dumps(result,indent=2)+"\n")
    print("\nRESULT",flush=True)
    if "rate_hz" in result:
        print(f"Report 0x{selected_id:02X}: {result['reports']}  Average: {result['rate_hz']:.2f} Hz",flush=True)
        gaps=result["interval_ms"]
        print(f"Intervals: median {gaps['median']:.3f} ms, p95 {gaps['p95']:.3f} ms, p99 {gaps['p99']:.3f} ms, max {gaps['max']:.3f} ms",flush=True)
    else: print("No measurable full-report stream; see captured report IDs.",flush=True)
    print(f"Changed inputs: {changes}; stick-motion reports: {motion}; timer repeats: {deltas.get(0,0) if selected_id==0x30 else 'n/a'}",flush=True)
    print(f"Saved: {output.resolve()}",flush=True)


if __name__=="__main__": main()
