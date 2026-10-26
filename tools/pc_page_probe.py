"""Try an unauthenticated Classic page to the receiver from Windows Bluetooth.

The receiver rejects this PC at its ACL admission filter. A received request
in its HCI trace proves paging succeeded even though RFCOMM never connects.
This does not pair, delete bonds, or send gamepad reports.
"""
from __future__ import annotations
import argparse
import ctypes as c
import json
from pathlib import Path
import socket  # Initialize Winsock for the process.
import time


class Address(c.Structure):
    _pack_ = 1
    _fields_ = [("family", c.c_ushort), ("address", c.c_uint64),
                ("service", c.c_ubyte * 16), ("port", c.c_ulong)]


class FDSet(c.Structure):
    _fields_ = [("count", c.c_uint), ("sockets", c.c_size_t * 64)]


class Timeval(c.Structure):
    _fields_ = [("seconds", c.c_long), ("microseconds", c.c_long)]


def probe(address: str) -> dict:
    compact = address.replace(":", "")
    if len(compact) != 12:
        raise ValueError("A six-byte Bluetooth address is required")
    remote = Address(32, int(compact, 16), (c.c_ubyte * 16)(), 1)
    winsock = c.WinDLL("ws2_32")
    winsock.socket.argtypes = [c.c_int, c.c_int, c.c_int]
    winsock.socket.restype = c.c_size_t
    winsock.ioctlsocket.argtypes = [c.c_size_t, c.c_long, c.POINTER(c.c_ulong)]
    winsock.connect.argtypes = [c.c_size_t, c.c_void_p, c.c_int]
    winsock.select.argtypes = [c.c_int, c.c_void_p, c.POINTER(FDSet), c.POINTER(FDSet), c.POINTER(Timeval)]
    winsock.getsockopt.argtypes = [c.c_size_t, c.c_int, c.c_int, c.c_void_p, c.POINTER(c.c_int)]
    winsock.closesocket.argtypes = [c.c_size_t]
    descriptor = winsock.socket(32, socket.SOCK_STREAM, 3)
    if descriptor == c.c_size_t(-1).value:
        raise OSError(winsock.WSAGetLastError(), "Bluetooth socket creation failed")
    start = time.monotonic()
    try:
        nonblocking = c.c_ulong(1)
        if winsock.ioctlsocket(descriptor, c.c_long(0x8004667e), c.byref(nonblocking)):
            raise OSError(winsock.WSAGetLastError(), "Cannot make socket nonblocking")
        result = winsock.connect(descriptor, c.byref(remote), c.sizeof(remote))
        initial = winsock.WSAGetLastError() if result else 0
        record = {"target": compact.lower(), "connect_return": result, "initial_error": initial}
        if initial in (0, 10035):
            write, error = FDSet(), FDSet()
            write.count = error.count = 1
            write.sockets[0] = error.sockets[0] = descriptor
            selected = winsock.select(0, None, c.byref(write), c.byref(error), c.byref(Timeval(12, 0)))
            record["select"] = selected
            if selected < 0:
                raise OSError(winsock.WSAGetLastError(), "Bluetooth select failed")
            if selected:
                status, length = c.c_int(), c.c_int(c.sizeof(c.c_int))
                if winsock.getsockopt(descriptor, 0xffff, 0x1007, c.byref(status), c.byref(length)):
                    raise OSError(winsock.WSAGetLastError(), "Cannot read connection result")
                record["socket_error"] = status.value
            else:
                record["probe_timeout"] = True
        record["seconds"] = round(time.monotonic() - start, 3)
        return record
    finally:
        winsock.closesocket(descriptor)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = probe(args.address)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
