#!/usr/bin/env python3

import argparse
import socket
import struct
import time


def parse_mcast_endpoint(text: str):
    if ":" not in text:
        raise ValueError("mcast endpoint must be <ipv4>:<port>")
    host, port_text = text.rsplit(":", 1)
    port = int(port_text)
    if port <= 0 or port > 65535:
        raise ValueError("port must be in [1, 65535]")
    return host, port


def build_frame(signature: bytes, seq: int) -> bytes:
    if not signature:
        signature = b"THEOS_RX_AUTOTEST"

    dst = b"\xFF\xFF\xFF\xFF\xFF\xFF"
    src = bytes((0x02, 0x00, 0x00, 0x00, 0x00, seq & 0xFF))
    ethertype = b"\x88\xB5"

    payload = signature + bytes((seq & 0xFF,))
    if len(payload) < (60 - 14):
        payload += bytes((0xA5,)) * ((60 - 14) - len(payload))

    return dst + src + ethertype + payload


def main():
    parser = argparse.ArgumentParser(description="Inject raw Ethernet frames into QEMU socket netdev mcast backend.")
    parser.add_argument("--mcast", required=True, help="Multicast endpoint in the form <ipv4>:<port>.")
    parser.add_argument("--delay-ms", type=int, default=4000, help="Delay before first injection in milliseconds.")
    parser.add_argument("--interval-ms", type=int, default=300, help="Interval between injections in milliseconds.")
    parser.add_argument("--count", type=int, default=3, help="Number of frames to inject.")
    parser.add_argument("--signature", default="THEOS_RX_AUTOTEST", help="ASCII signature written in frame payload.")
    args = parser.parse_args()

    host, port = parse_mcast_endpoint(args.mcast)
    signature = args.signature.encode("ascii", "ignore")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("B", 1))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, struct.pack("B", 1))

    if args.delay_ms > 0:
        time.sleep(args.delay_ms / 1000.0)

    for i in range(args.count):
        frame = build_frame(signature, i)
        sock.sendto(frame, (host, port))
        if i + 1 < args.count and args.interval_ms > 0:
            time.sleep(args.interval_ms / 1000.0)


if __name__ == "__main__":
    main()
