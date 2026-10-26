"""Short left/right/both rumble pulses through interrupt OUT and Windows SET_REPORT.

Selects only the custom diagnostic. Sends explicit neutral stops in finally.
Keeps Nintendo's two four-byte HD payloads intact; no host amplitude downmix.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import time
import hid
from diagnostic_hid import receiver_devices
from diagnostic_hid import exchange


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out",type=Path)
    args=parser.parse_args()
    devices=receiver_devices()
    if len(devices)!=1: raise RuntimeError("Expected one custom diagnostic")
    handle=hid.device(); handle.open_path(devices[0]["path"])
    kernel=ctypes.WinDLL("kernel32",use_last_error=True)
    hid_dll=ctypes.WinDLL("hid",use_last_error=True)
    kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,
                                wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
    kernel.CreateFileW.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    hid_dll.HidD_SetOutputReport.argtypes=[wintypes.HANDLE,ctypes.c_void_p,wintypes.ULONG]
    hid_dll.HidD_SetOutputReport.restype=wintypes.BOOLEAN
    path=devices[0]["path"]
    if isinstance(path,bytes): path=path.decode("utf-8")
    control=kernel.CreateFileW(path,0xc0000000,3,None,3,0,None)
    if control==ctypes.c_void_p(-1).value:
        handle.close(); raise ctypes.WinError(ctypes.get_last_error())
    counter=0
    neutral=bytes.fromhex("00 01 40 40")
    pulse=bytes.fromhex("00 41 40 50")
    def send(payload,via_control=False,subcommand=None):
        nonlocal counter
        report=bytearray(64); report[0]=0x10 if subcommand is None else 1
        report[1]=counter&15; counter+=1; report[2:10]=payload
        if subcommand is not None: report[10:12]=bytes(subcommand)
        if via_control:
            data=ctypes.create_string_buffer(bytes(report))
            if not hid_dll.HidD_SetOutputReport(control,data,64):
                raise ctypes.WinError(ctypes.get_last_error())
        elif handle.write(report)!=64: raise RuntimeError("Incomplete HID output")
    result={"before":exchange(handle,0xf9),"phases":[]}
    try:
        status=exchange(handle,0xf4)
        if status["connected"]!=1 or status["setup_stage"]!=10:
            raise RuntimeError("Wake the saved Pro before this probe")
        send(neutral*2,subcommand=(0x48,1)); time.sleep(.1)
        for via_control in (False,True):
            source="control" if via_control else "interrupt"
            for label,payload in (("left",pulse+neutral),("right",neutral+pulse),("both",pulse*2)):
                print(f"{source}: {label}",flush=True)
                before=exchange(handle,0xf9)
                for _ in range(30):
                    send(payload,via_control); time.sleep(.02)
                time.sleep(.05)
                after=exchange(handle,0xf9)
                result["phases"].append({"source":source,"side":label,"payload":payload.hex(),
                    "before":before,"after":after})
                send(neutral*2,via_control); time.sleep(.25)
    finally:
        try:
            send(neutral*2); time.sleep(.05)
            result["after"]=exchange(handle,0xf9)
            if args.out: args.out.write_text(json.dumps(result,indent=2)+"\n")
        finally:
            kernel.CloseHandle(control); handle.close()
    print(json.dumps(result,indent=2))


if __name__=="__main__": main()
