"""Ping only the custom 8BitDo HID Diagnostic; optionally request original BL."""
from __future__ import annotations
import argparse
import hashlib
import json
import secrets
import struct
import time

TIMING_MODES = ["active", "sniff5", "sniff7.5", "sniff10", "sniff15", "qos8", "flow8", "sniff500"]
TIMING_STATES = ["none", "pending", "exiting", "entering", "qos", "done", "failed", "policy", "flow"]
PACKET_MODES = ["all", "one-slot", "br-three-slot", "three-slot", "br-only"]
PAGE_SCAN_MODES = ["read", "fast", "restore"]

def exchange(handle, operation, variant=0, seconds=10):
    nonce = secrets.token_bytes(4)
    report = bytearray(64)
    report[:6] = bytes([0x80, operation]) + b"8BDI"
    if operation == 0xf1:
        report[6:10] = b"BOOT"
    elif operation == 0xf3:
        report[6:10] = b"RDA!"
        report[14] = variant
    elif operation == 0xf5:
        report[6:10] = b"PAIR"
    elif operation == 0xf6:
        report[6:10] = b"LEDS"
        report[14]=variant; report[15]=seconds
    elif operation == 0xf7:
        struct.pack_into("<H",report,14,variant)
    elif operation == 0xf8:
        report[6:10]=b"SLOT"; report[14]=variant
    elif operation==0xfa:
        report[14]=variant
    elif operation==0xfb:
        report[6:10]=b"LINK"; report[14]=variant
    elif operation==0xfc:
        report[6:10]=b"TIME"; report[14]=variant
    elif operation==0xfd:
        report[6:10]=b"POLL"; report[14]=variant; report[15]=seconds
    elif operation==0xfe:
        report[6:10]=b"PKTS"; report[14]=variant
    elif operation==0xff:
        report[6:10]=b"SCAN" if variant<3 else b"ACQR"; report[14]=variant
    report[10:14] = nonce
    if handle.write(report) != 64:
        raise RuntimeError("Incomplete numbered HID report write")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        reply = bytes(handle.read(64, 300))
        if len(reply) == 64 and reply[:6] == bytes([0x81, operation]) + b"8BDI" and reply[32:36] == nonce:
            if operation==0xfe:
                return {"packet_mode":PACKET_MODES[reply[6]]}
            if operation==0xfd:
                return {"output_probe_ms":reply[6],"seconds":reply[7]}
            if operation==0xfc:
                return {"requested_timing":TIMING_MODES[reply[6]]}
            if operation==0xfb:
                return {"requested_role":reply[6]}
            if operation==0xfa:
                if variant==8:
                    return {"acquisition_probe":{"active":bool(reply[6]),
                            "hid_cid":struct.unpack_from("<H",reply,8)[0],
                            "remaining_ms":struct.unpack_from("<I",reply,12)[0],
                            "peers":[dict(zip(['kind','actual_mode','requested_mode','pending','waiting','expected'],reply[o:o+6]))
                                     for o in (16,24)]}}
                if variant==7:
                    if reply[11]==0xa4:
                        interval,window=struct.unpack_from("<2H",reply,12)
                        return {"scan_readback":{"phase":reply[6],"status":reply[7],
                                "links":reply[8],"enable":reply[9],"type":reply[10],
                                "interval_slots":interval,"window_slots":window}}
                    interval,window,original_interval,original_window=struct.unpack_from("<4H",reply,12)
                    return {"page_scan_probe":{"mode":PAGE_SCAN_MODES[reply[6]],"state":reply[7],
                            "status":reply[8],"original_valid":bool(reply[9]),"type":reply[10],
                            "original_type":reply[11],"interval_slots":interval,"window_slots":window,
                            "original_interval_slots":original_interval,"original_window_slots":original_window}}
                if variant==6:
                    peers=[]
                    for index,offset in enumerate((8,36)):
                        data=reply[offset:offset+24]
                        if not data[0]: continue
                        cid,acl_handle=struct.unpack_from("<HH",data,6)
                        peers.append({"index":index,"saved_kind":data[0],"opened":bool(data[1]),
                                      "ready":bool(data[2]),"setup_stage":data[3],"error":data[4],
                                      "key_loaded":bool(data[5]),"hid_cid":cid,"handle":acl_handle,
                                      "actual_kind":data[10],"input_valid":bool(data[11]),
                                      "full_reports":struct.unpack_from("<I",data,12)[0],
                                      "address":data[16:22].hex(),"output_busy":bool(data[22]),"connection_state":data[23]})
                    return {"active_slot":reply[6],"slot_kind":reply[7],"peers":peers,
                            "joycon_page_scan":{"before":reply[60],"after":reply[61],
                                                "state":reply[62],"status":reply[63]}}
                if variant==5:
                    handle,requested,reported=struct.unpack_from("<HHH",reply,10)
                    return {"packet_probe":{"mode":PACKET_MODES[reply[6]],
                            "state":["idle","queued","waiting","done","failed"][reply[7]],
                            "status":reply[8],"handle":handle,"requested_mask":requested,
                            "reported_mask":reported,"mask_confirmed":reply[7]==3 and requested==reported,
                            "requested_at_ms":struct.unpack_from("<I",reply,16)[0],
                            "local_usable_mask":struct.unpack_from("<H",reply,20)[0]}}
                if variant==4:
                    values=struct.unpack_from("<6I",reply,8)+struct.unpack_from("<3I",reply,36)
                    probe=dict(zip(["packets","intervals","total_us","min_us","max_us",
                                    "under_7ms","near_8ms","over_9ms","remaining_ms"],values))
                    probe.update(active=bool(reply[6]),interval_ms=reply[7],
                                 actual_rate_hz=values[1]*1e6/values[2] if values[2] else None)
                    return {"output_probe":probe}
                if variant==3:
                    links=[]
                    for offset in (8,36):
                        data=reply[offset:offset+24]
                        if not data[0]: continue
                        handle,interval=struct.unpack_from("<HH",data,4)
                        reports,requested_at,qos_latency=struct.unpack_from("<III",data,12)
                        links.append({"ready":bool(data[1]),"role":data[2],"mode":data[3],
                                      "handle":handle,"interval_slots":interval,"interval_ms":interval*.625,
                                      "requested_timing":TIMING_MODES[data[8]],
                                      "state":TIMING_STATES[data[9]],"status":data[10],
                                      "link_policy":data[11],
                                      "full_reports":reports,"requested_at_ms":requested_at,
                                      "qos_latency_us":qos_latency})
                    return {"timing_preference":TIMING_MODES[reply[6]],"links":links}
                values=struct.unpack_from("<6I",reply,8)+struct.unpack_from("<7I",reply,36)
                if variant==2:
                    return {"uart_idle":dict(zip(["bursts","intervals","total_us","max_us","rx_high_water"],values[:5]))}
                if variant:
                    return {"timer_delta_histogram":dict(zip(["0","1","2","3","4","5","6","7+"],values[:8]))}
                names=["hci_acl_packets","hid_full_reports","published","duplicate_timers","short_reports",
                       "publish_rejected","timer_ticks","timer_steps","max_timer_step","h4_bad_types",
                       "h4_oversized","uart_errors","dma_errors"]
                return {"role":reply[6],"link_mode":reply[7],**dict(zip(names,values))}
            if operation==0xf9:
                return {"vibration_enabled":bool(reply[6]),"imu_enabled":bool(reply[7]),
                        "streaming":bool(reply[8]),"active_slot":reply[9],
                        "rumble_sequence":struct.unpack_from("<I",reply,16)[0],
                        "usb_rumble":reply[20:28].hex(),
                        **dict(zip(["bt_output_reports","bt_rumble_reports","control_reports","interrupt_reports"],
                                   struct.unpack_from("<4I",reply,36))),"bt_rumble":reply[52:60].hex()}
            if operation==0xf8:
                return {"requested_slot":reply[6]}
            if operation==0xff:
                if reply[6]>=3: return {"requested_acquisition":"quiet" if reply[6]==3 else "full"}
                return {"requested_page_scan":PAGE_SCAN_MODES[reply[6]]}
            if operation==0xf7:
                names=["unused","command","complete","status","acl_open","acl_close","auth",
                       "key_get","key_put","admit","hid_open","hid_close","epoch","connect","save","usb",
                       "role","mode","role_request","timing_request","qos","flow","discover"]
                data=reply[8:32]+reply[36:60]
                records=[]
                for i in range(reply[6]):
                    ms,event,status,value=struct.unpack_from("<IBBH",data,i*8)
                    entry={"ms":ms,"event":names[event] if event<len(names) else event,
                           "status":status,"value":value}
                    if event==15:
                        entry.update(report_id=f"{status:02x}",command=f"{value>>8:02x}",length=value&255)
                    records.append(entry)
                cursor,end=struct.unpack_from("<HH",reply,60)
                return {"cursor":cursor,"end":end,"records":records}
            if operation==0xf6:
                return {"led_test_channel":variant,"seconds":seconds,"reply_valid":True}
            if operation in (0xf4,0xf5):
                return {**dict(zip(["state","error","pairing","connected","setup_stage","setup_error",
                                    "led_duty","active_slot","slot_kind","button_pressed"],reply[6:16])),
                        **dict(zip(["inquiry_results","opens","closes","last_cid","last_opcode"],
                                   struct.unpack_from("<5H",reply,16))),
                        "peer_address":reply[26:32].hex(),
                        "reports":struct.unpack_from("<I",reply,36)[0],
                        "published":struct.unpack_from("<I",reply,40)[0],
                        "last_report_prefix":reply[44:60].hex(),
                        "led_channel":reply[60],"storage_error":reply[61]}
            if operation in (0xf2, 0xf3):
                names = ["off", "power_low", "rf_first", "power_pause", "power_release", "rf_second",
                         "core", "rf_tail", "patch", "conditional", "conditional_tail", "identity",
                         "baud", "set_baud", "query", "ready", "error"]
                words = struct.unpack_from("<8H", reply, 10)
                counts = struct.unpack_from("<4I", reply, 44)
                return {"phase": names[reply[6]] if reply[6] < len(names) else reply[6],
                        "error": reply[7], "variant": reply[8], "hci_version": reply[9],
                        **dict(zip(["row", "i2c_writes", "hci_writes", "events", "manufacturer",
                                    "lmp_subversion", "acl_length", "acl_buffers"], words)),
                        "address_hci_hex": reply[26:32].hex(), "features_hex": reply[36:44].hex(),
                        **dict(zip(["baud", "rx_bytes", "tx_bytes", "overruns"], counts)),
                        "last_event_length": reply[60], "last_event_prefix": reply[61:64].hex()}
            return {"reply_valid": True, "protocol": reply[6], "boot_requested": bool(reply[7]),
                    "uptime_ms": struct.unpack_from("<I", reply, 8)[0],
                    "firmware_version": struct.unpack_from("<I", reply, 12)[0],
                    "clock_hz": struct.unpack_from("<I", reply, 16)[0],
                    "uid_sha256": hashlib.sha256(reply[20:32]).hexdigest()}
    raise RuntimeError("No matching diagnostic response")

def receiver_devices():
    import hid
    return [d for d in hid.enumerate(0x057e, 0)
            if d.get("product_id") in (0x2009, 0x200e, 0x20ff)
            and d.get("product_string") == "8BitDo HID Diagnostic"]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bootloader", action="store_true")
    p.add_argument("--out", type=str)
    p.add_argument("--radio-status", action="store_true")
    p.add_argument("--radio-start", type=int, choices=[0,1])
    p.add_argument("--host-status", action="store_true")
    p.add_argument("--pair", action="store_true", help="Start the same pairing window as a button hold")
    p.add_argument("--slot",type=int,choices=[1,2],help="Select a profile through the diagnostic interface")
    p.add_argument("--trace",action="store_true",help="Read bounded non-secret Bluetooth event metadata")
    p.add_argument("--output-status",action="store_true",help="Inspect USB and Bluetooth rumble forwarding")
    p.add_argument("--rate-status",action="store_true",help="Read HCI/HID publication and timer counters")
    p.add_argument("--role",choices=["master","slave"],help="Set the incoming Classic role preference and request a live switch; verify actual role with --rate-status")
    p.add_argument("--timing",choices=TIMING_MODES,help="Negotiate Classic link timing; verify completion and actual interval with --rate-status")
    p.add_argument("--output-probe-ms",type=int,choices=range(256),help="Temporarily resend current rumble on the ready Pro; 0 stops")
    p.add_argument("--output-probe-seconds",type=int,choices=range(1,256),default=15)
    p.add_argument("--packet-mode",choices=PACKET_MODES,help="One-shot standard HCI packet mask experiment")
    p.add_argument("--page-scan",choices=PAGE_SCAN_MODES,help="Read, temporarily select fast interlaced scan, or restore original settings")
    p.add_argument("--acquisition",choices=['quiet','full'],help="Temporarily quiet the sole Joy-Con's full-report stream (20 s maximum), or restore full mode")
    p.add_argument("--led-channel",type=int,choices=[0,1,2,4,7])
    p.add_argument("--led-seconds",type=int,choices=range(31),default=10)
    args = p.parse_args()
    import hid
    devices = receiver_devices()
    if len(devices) != 1:
        raise RuntimeError(f"Expected exactly one custom diagnostic; found {len(devices)}")
    handle = hid.device(); handle.open_path(devices[0]["path"]); handle.set_nonblocking(0)
    result = {"vid": "057e", "pid": format(devices[0]["product_id"],"04x"), "product": devices[0]["product_string"]}
    try:
        result["ping"] = exchange(handle, 0xf0)
        if result["ping"]["firmware_version"]>=0x10019 and any((
            args.role is not None, args.timing is not None, args.output_probe_ms is not None,
            args.packet_mode is not None, args.page_scan is not None, args.acquisition is not None)):
            raise RuntimeError("Firmware 0x10019 retired the experimental role/timing/scan/report probes")
        if args.radio_start is not None:
            result["radio_start"] = exchange(handle, 0xf3, args.radio_start)
        if args.radio_status:
            result["radio"] = exchange(handle, 0xf2)
        if args.pair:
            result["pair"] = exchange(handle,0xf5)
        if args.slot is not None:
            result["slot"]=exchange(handle,0xf8,args.slot-1)
        if args.host_status:
            result["host"] = exchange(handle,0xf4)
            if result["ping"]["firmware_version"]>=0x10013:
                result["controllers"]=exchange(handle,0xfa,6)
        if args.output_status:
            result["output"]=exchange(handle,0xf9)
        if args.role is not None:
            result["role_request"]=exchange(handle,0xfb,0 if args.role=="master" else 1)
        if args.timing is not None:
            if result["ping"]["firmware_version"]<0x1000d:
                raise RuntimeError("Timing control requires firmware 0x1000d or later")
            if args.timing=="flow8" and result["ping"]["firmware_version"]<0x1000f:
                raise RuntimeError("Incoming flow control requires firmware 0x1000f or later")
            if args.timing=="sniff500" and result["ping"]["firmware_version"]<0x10015:
                raise RuntimeError("Long sniff diagnostic requires firmware 0x10015 or later")
            result["timing_request"]=exchange(handle,0xfc,TIMING_MODES.index(args.timing))
        if args.output_probe_ms is not None:
            if result["ping"]["firmware_version"]<0x10010:
                raise RuntimeError("Output-cadence probe requires firmware 0x10010 or later")
            result["probe_request"]=exchange(handle,0xfd,args.output_probe_ms,args.output_probe_seconds)
        if args.packet_mode is not None:
            if result["ping"]["firmware_version"]<0x10011:
                raise RuntimeError("Packet-mask probe requires firmware 0x10011 or later")
            result["packet_request"]=exchange(handle,0xfe,PACKET_MODES.index(args.packet_mode))
        if args.page_scan is not None:
            if result["ping"]["firmware_version"]<0x10017:
                raise RuntimeError("Page scan probe requires firmware 0x10017 or later")
            result["page_scan_request"]=exchange(handle,0xff,PAGE_SCAN_MODES.index(args.page_scan))
            deadline=time.monotonic()+4
            while True:
                result["page_scan"]=exchange(handle,0xfa,7)["page_scan_probe"]
                if result["page_scan"]["state"] in (13,14) or time.monotonic()>=deadline: break
                time.sleep(.03)
        if args.rate_status:
            result["rate"]={**exchange(handle,0xfa),**exchange(handle,0xfa,1)}
            if result["ping"]["firmware_version"]>=0x10024:
                result["rate"].update(exchange(handle,0xfa,7))
            if result["ping"]["firmware_version"]>=0x1000b:
                result["rate"].update(exchange(handle,0xfa,2))
            if result["ping"]["firmware_version"]>=0x1000d:
                result["rate"].update(exchange(handle,0xfa,3))
            if 0x10010<=result["ping"]["firmware_version"]<0x10019:
                result["rate"].update(exchange(handle,0xfa,4))
            if 0x10011<=result["ping"]["firmware_version"]<0x10019:
                result["rate"].update(exchange(handle,0xfa,5))
            if 0x10017<=result["ping"]["firmware_version"]<0x10019:
                result["rate"].update(exchange(handle,0xfa,7))
            if 0x10018<=result["ping"]["firmware_version"]<0x10019:
                result["rate"].update(exchange(handle,0xfa,8))
        if args.acquisition is not None:
            if result["ping"]["firmware_version"]<0x10018:
                raise RuntimeError("Acquisition probe requires firmware 0x10018 or later")
            result['acquisition_request']=exchange(handle,0xff,3 if args.acquisition=='quiet' else 4)
        if args.led_channel is not None:
            result["led_test"]=exchange(handle,0xf6,args.led_channel,args.led_seconds)
        if args.trace:
            cursor=0xffff; records=[]
            for _ in range(24):
                page=exchange(handle,0xf7,cursor)
                records.extend(page["records"])
                cursor=(page["cursor"]+len(page["records"]))&0xffff
                if cursor==page["end"] or not page["records"]:
                    break
            result["trace"]=records
        if args.bootloader:
            result["return_command"] = exchange(handle, 0xf1)
    finally:
        handle.close()
    if args.bootloader:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            boot = hid.enumerate(0x0483, 0x5760)
            diagnostic = receiver_devices()
            if len(boot) == 1 and not diagnostic:
                result["original_bootloader_reenumerated"] = True
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Original bootloader did not reenumerate")
    if args.out:
        from pathlib import Path
        Path(args.out).write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
