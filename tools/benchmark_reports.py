"""Measure custom receiver USB delivery and independent Bluetooth publications.

Temporarily changes only the host IMU request and restores it on exit. HID read
timestamps measure Windows delivery, not wire timing; use --pcap for USBPcap.
"""
from __future__ import annotations
from diagnostic_hid import receiver_devices
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
import subprocess
import time

from diagnostic_hid import exchange
from report_patterns import patterns


def timing(stamps: list[float]) -> dict:
    if len(stamps)<2:
        raise RuntimeError("Need at least two full input reports")
    gaps=[(b-a)*1000 for a,b in zip(stamps,stamps[1:])]
    ordered=sorted(gaps)
    def percentile(p):
        return ordered[round((len(ordered)-1)*p)]
    return {"reports":len(stamps),"span_seconds":stamps[-1]-stamps[0],
            "rate_hz":(len(stamps)-1)/(stamps[-1]-stamps[0]),
            "interval_ms":{"mean":statistics.mean(gaps),"median":statistics.median(gaps),
                           "p95":percentile(.95),"p99":percentile(.99),"max":max(gaps),
                           "under_1ms":sum(g<1 for g in gaps),
                           "over_12ms":sum(g>12 for g in gaps),
                           "rounded_histogram":dict(sorted(Counter(round(g) for g in gaps).items()))}}


def pcap_result(path: Path, tshark: str) -> dict:
    rows=subprocess.check_output([tshark,"-r",str(path),"-Y",
        "usb.endpoint_address == 0x81 && usb.data_len == 64","-T","fields",
        "-e","frame.time_relative","-e","usbhid.data"],text=True)
    stamps=[float(row.split("\t")[0]) for row in rows.splitlines()
            if row.split("\t")[-1].startswith("30")]
    return {"capture":str(path),"clock":"USBPcap transfer completion",**timing(stamps)}


def rate_counters(h) -> dict:
    result = {}
    for page in (0, 1, 2, 3):
        result.update(exchange(h, 0xfa, page))
    return result

def live_result(seconds: int, mode: str) -> dict:
    import hid
    devices=receiver_devices()
    if len(devices)!=1: raise RuntimeError("Expected exactly one custom receiver")
    h=hid.device(); h.open_path(devices[0]["path"]); h.set_nonblocking(0)
    result={"clock":"Windows HID read completion","phases":[]}
    initial=None
    counter=0
    def imu(enabled):
        nonlocal counter
        report=bytearray(64); report[0]=1; report[1]=counter&15; counter+=1
        report[2:10]=bytes.fromhex("00 01 40 40 00 01 40 40")
        report[10]=0x40; report[11]=int(enabled)
        if h.write(report)!=64: raise RuntimeError("Incomplete IMU request")
        until=time.monotonic()+3
        while time.monotonic()<until:
            reply=bytes(h.read(64,100))
            if len(reply)==64 and reply[0]==0x21 and reply[14]==0x40:
                if not reply[13]&0x80: raise RuntimeError("IMU request NACK")
                return
        raise RuntimeError("No IMU acknowledgement")
    try:
        result["ping"]=exchange(h,0xf0)
        initial=exchange(h,0xf9)
        if not initial["streaming"]: raise RuntimeError("Full input mode is not running")
        for enabled in ([False,True] if mode=="both" else [mode=="on"]):
            imu(enabled)
            warmup=time.monotonic()+1
            while time.monotonic()<warmup: h.read(64,100)
            before=exchange(h,0xf4)
            if before["connected"]!=1 or before["setup_stage"]!=10:
                raise RuntimeError("Expected one connected, ready Pro")
            before_time=time.perf_counter()
            before_rate=rate_counters(h); before_rate_time=time.perf_counter()
            h.set_nonblocking(1)
            while h.read(64): pass
            h.set_nonblocking(0)
            stamps=[]; timers=[]; samples=[]; changed_inputs=0; stick_motion_reports=0; previous=None
            started=time.perf_counter(); end=started+seconds
            while time.perf_counter()<end:
                report=bytes(h.read(64,100)); stamp=time.perf_counter()
                if len(report)==64 and report[0]==0x30 and stamp<=end:
                    stamps.append(stamp); timers.append(report[1])
                    samples.append({"elapsed_ms":(stamp-started)*1000,"report_hex":report.hex()})
                    if previous is not None:
                        changed_inputs+=report[3:12]!=previous[3:12]
                        def axes(data):
                            return [data[o]|((data[o+1]&15)<<8) for o in (6,9)]+[(data[o+1]>>4)|(data[o+2]<<4) for o in (6,9)]
                        stick_motion_reports+=any(abs(a-b)>=32 for a,b in zip(axes(report),axes(previous)))
                    previous=report
            after=exchange(h,0xf4); after_time=time.perf_counter()
            after_rate=rate_counters(h); after_rate_time=time.perf_counter()
            output=exchange(h,0xf9)
            if output["imu_enabled"]!=enabled:
                raise RuntimeError("Another host changed the IMU request during measurement")
            phase={"imu_enabled":enabled,**timing(stamps),
                   "changed_inputs":changed_inputs,"stick_motion_reports":stick_motion_reports,
                   "repeated_report_timers":sum(a==b for a,b in zip(timers,timers[1:])),
                   "bt_published_delta":(after["published"]-before["published"])&0xffffffff,
                   "bt_observation_seconds":after_time-before_time,
                   "host_before":before,"host_after":after,"output_after":output}
            phase["bt_publication_hz"]=phase["bt_published_delta"]/phase["bt_observation_seconds"]
            phase["rate_before"]=before_rate; phase["rate_after"]=after_rate
            phase["bt_raw_hid_hz"]=((after_rate["hid_full_reports"]-before_rate["hid_full_reports"])&0xffffffff)/(after_rate_time-before_rate_time)
            phase["timer_delta_histogram"]={k:after_rate["timer_delta_histogram"][k]-v for k,v in before_rate["timer_delta_histogram"].items()}
            count=after_rate["uart_idle"]["intervals"]-before_rate["uart_idle"]["intervals"]
            total=(after_rate["uart_idle"]["total_us"]-before_rate["uart_idle"]["total_us"])&0xffffffff
            phase["uart_idle_interval_us"]=total/count if count else None
            result["phases"].append(phase)
            print(json.dumps({k:v for k,v in phase.items() if k not in ("host_before","host_after","rate_before","rate_after","output_after")}),flush=True)
            phase["samples"]=samples
            phase["patterns"]=patterns(samples)
            print(json.dumps({"patterns":phase["patterns"]}),flush=True)
    finally:
        try:
            if initial is not None: imu(initial["imu_enabled"])
        finally: h.close()
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds",type=int,choices=range(5,121),default=30)
    parser.add_argument("--imu",choices=["off","on","both"],default="both")
    parser.add_argument("--pcap",type=Path)
    parser.add_argument("--tshark",default="tshark")
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    result=pcap_result(args.pcap,args.tshark) if args.pcap else live_result(args.seconds,args.imu)
    args.out.write_text(json.dumps(result,indent=2)+"\n")
    if args.pcap: print(json.dumps(result,indent=2))


if __name__=="__main__": main()
