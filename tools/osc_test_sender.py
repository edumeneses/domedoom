#!/usr/bin/env python3
"""
DomeDoom OSC control — minimal test sender (no dependencies).

Usage:
    ./osc_test_sender.py [host] [port] /domedoom/front 1.0
    ./osc_test_sender.py 127.0.0.1 6666 /domedoom/rotate 90

Sends a single OSC message. Enable reception in-game with `osc_control 1`
(Options -> OSC Control) on the target host.
"""
import socket
import struct
import sys


def pad4(b: bytes) -> bytes:
    return b + b"\x00" * (4 - (len(b) % 4) if len(b) % 4 else 4)


def osc_message(addr: str, arg=None) -> bytes:
    msg = pad4(addr.encode())
    if arg is None:
        msg += pad4(b",")
    elif isinstance(arg, int):
        msg += pad4(b",i") + struct.pack(">i", arg)
    else:
        msg += pad4(b",f") + struct.pack(">f", float(arg))
    return msg


def main() -> None:
    args = sys.argv[1:]
    host, port = "127.0.0.1", 6666
    # optional leading host/port before the address
    while args and not args[0].startswith("/"):
        if args[0].replace(".", "").isdigit() and "." in args[0]:
            host = args.pop(0)
        elif args[0].isdigit():
            port = int(args.pop(0))
        else:
            break
    if not args:
        print(__doc__)
        sys.exit(1)
    addr = args[0]
    arg = None
    if len(args) > 1:
        v = args[1]
        arg = int(v) if v.lstrip("-").isdigit() else float(v)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(osc_message(addr, arg), (host, port))
    print(f"sent {addr} {arg if arg is not None else ''} -> {host}:{port}")


if __name__ == "__main__":
    main()
