"""Exercise Nintendo USB initialization and collect bounded full input reports.

Targets only the custom diagnostic product, never a physical Nintendo USB pad.
Rumble stays neutral. Leaves full input mode and host IMU request enabled.
"""
import argparse
import json
from pathlib import Path
import struct
import time
import hid
from diagnostic_hid import receiver_devices


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds",type=int,choices=range(1,61),default=10)
    parser.add_argument("--out",type=Path)
    args=parser.parse_args()
    devices=receiver_devices()
    if len(devices)!=1: raise RuntimeError("Expected one custom diagnostic")
    handle=hid.device(); handle.open_path(devices[0]["path"])
    counter=0
    def send(report,reply_id,command):
        if handle.write(report)!=64: raise RuntimeError("Incomplete HID output")
        if reply_id is None: return
        until=time.monotonic()+3
        while time.monotonic()<until:
            reply=bytes(handle.read(64,200))
            if len(reply)==64 and reply[0]==reply_id and reply[1 if reply_id==0x81 else 14]==command:
                if reply_id==0x21 and not reply[13]&0x80: raise RuntimeError(f"Subcommand {command:02x} NACK")
                return
        raise RuntimeError(f"Missing reply to {command:02x}")
    def subcommand(command,data):
        nonlocal counter
        report=bytearray(64); report[0]=1; report[1]=counter&15; counter+=1
        report[2:10]=bytes.fromhex("00 01 40 40 00 01 40 40")
        report[10]=command; report[11:11+len(data)]=data
        send(report,0x21,command)
    result={"usb_handshake":False,"input_reports":0,"button_states":[],"sticks_min":[4095]*4,
            "sticks_max":[0]*4,"imu_min":[32767]*6,"imu_max":[-32768]*6}
    try:
        for command in (1,2,3,2,4):
            report=bytearray(64); report[:2]=bytes([0x80,command])
            send(report,None if command==4 else 0x81,command)
        subcommand(2,b""); subcommand(0x40,b"\x01"); subcommand(3,b"\x30")
        result["usb_handshake"]=True
        states=set(); started=time.monotonic()
        while time.monotonic()-started<args.seconds:
            report=bytes(handle.read(64,200))
            if len(report)!=64 or report[0]!=0x30: continue
            result["input_reports"]+=1; states.add(report[3:6].hex())
            sticks=[]
            for offset in (6,9):
                sticks.extend((report[offset]|((report[offset+1]&15)<<8),
                               (report[offset+1]>>4)|(report[offset+2]<<4)))
            for i,value in enumerate(sticks):
                result["sticks_min"][i]=min(result["sticks_min"][i],value)
                result["sticks_max"][i]=max(result["sticks_max"][i],value)
            for frame in range(3):
                for i,value in enumerate(struct.unpack_from("<6h",report,13+frame*12)):
                    result["imu_min"][i]=min(result["imu_min"][i],value)
                    result["imu_max"][i]=max(result["imu_max"][i],value)
        result["button_states"]=sorted(states); result["elapsed_seconds"]=round(time.monotonic()-started,2)
    finally: handle.close()
    if args.out: args.out.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))


if __name__=="__main__": main()
