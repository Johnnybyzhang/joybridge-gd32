"""Direct Bluetooth mode negotiation and both Nintendo input layouts."""
import sys
from pathlib import Path
import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from benchmark_direct_bt import initialize_full_mode,sticks


class Pro:
    def __init__(self,ack=0x80):
        self.ack=ack; self.output=[]; self.pending=[]

    def write(self,report):
        report=bytes(report); self.output.append(report)
        reply=bytearray(128); reply[0]=0x21; reply[13]=self.ack; reply[14]=report[10]
        self.pending.extend([bytes([0x3f])+bytes(127),bytes(reply)])
        if report[10]==3: self.pending.append(bytes([0x30])+bytes(127))
        return len(report)

    def read(self,size,timeout):
        return self.pending.pop(0) if self.pending else b""


def test_initialize_from_simple_mode_with_unsolicited_reports():
    pro=Pro()
    assert initialize_full_mode(pro)==[{"command":"40","ack":"80"},{"command":"03","ack":"80"}]
    assert len(pro.output)==2
    assert all(len(report)==49 and report[0]==1 for report in pro.output)
    assert pro.output[0][10:12]==bytes([0x40,1])
    assert pro.output[1][10:12]==bytes([3,0x30])
    assert all(report[2:10]==bytes.fromhex("0001404000014040") for report in pro.output)


def test_nack_prevents_claiming_full_mode():
    pro=Pro(ack=0)
    with pytest.raises(RuntimeError,match="NACK"):
        initialize_full_mode(pro)
    assert len(pro.output)==1


def test_simple_and_full_stick_layouts():
    simple=bytearray(128); simple[0]=0x3f
    for offset,value in zip((4,6,8,10),(0,32767,32768,65535)):
        simple[offset:offset+2]=value.to_bytes(2,"little")
    assert sticks(simple)==[0,32767,32768,65535]
    full=bytearray(128); full[0]=0x30
    full[6:12]=bytes.fromhex("00f0ff ff0780")
    assert sticks(full)==[0,4095,2047,2048]
