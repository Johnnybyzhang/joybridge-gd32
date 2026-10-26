"""HCI completion must not be mistaken for confirmation of a requested mask."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from diagnostic_hid import exchange


@pytest.mark.parametrize("state,requested,reported,confirmed",[
    (3,0xee1c,0xee1c,True),
    (3,0x331c,0xee1c,False),  # live RDA response: success with original mask
    (2,0xee1c,0xee1c,False),  # a prior matching result does not confirm a new request
    (4,0xee1c,0xee1c,False),
])
def test_completion_and_mask_confirmation(state,requested,reported,confirmed):
    class Handle:
        def write(self,report):
            self.nonce=report[10:14]
            assert report[:6]==b"\x80\xfa8BDI" and report[14]==5
            return len(report)

        def read(self,*_):
            reply=bytearray(64)
            reply[:6]=b"\x81\xfa8BDI"; reply[7]=state
            struct.pack_into("<HHH",reply,10,50,requested,reported)
            reply[32:36]=self.nonce
            return reply

    result=exchange(Handle(),0xfa,5)["packet_probe"]
    assert result["mask_confirmed"] is confirmed
    assert result["requested_mask"]==requested and result["reported_mask"]==reported
