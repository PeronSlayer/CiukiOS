#!/usr/bin/env python3
"""Inject an Ethernet ICMP echo request into a QEMU multicast LAN."""

from __future__ import annotations

import argparse
import ipaddress
import socket
import struct
import sys
import time


HOST_MAC = bytes.fromhex("525400aabb01")
GUEST_MAC = bytes.fromhex("525400123456")
IDENTIFIER = 0xC1C1
SEQUENCE = 1
PAYLOAD = b"CiukiOS inbound ICMP gate"


def checksum(payload: bytes) -> int:
    if len(payload) & 1:
        payload += b"\x00"
    total = sum(struct.unpack(f"!{len(payload) // 2}H", payload))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet_frame(payload: bytes, destination: bytes, ethertype: int) -> bytes:
    return (destination + HOST_MAC + struct.pack("!H", ethertype) + payload).ljust(60, b"\x00")


def arp_request(host_ip: bytes, guest_ip: bytes) -> bytes:
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1,
        0x0800,
        6,
        4,
        1,
        HOST_MAC,
        host_ip,
        b"\x00" * 6,
        guest_ip,
    )
    return ethernet_frame(arp, b"\xff" * 6, 0x0806)


def echo_request(host_ip: bytes, guest_ip: bytes) -> bytes:
    icmp = struct.pack("!BBHHH", 8, 0, 0, IDENTIFIER, SEQUENCE) + PAYLOAD
    icmp = icmp[:2] + struct.pack("!H", checksum(icmp)) + icmp[4:]
    total_length = 20 + len(icmp)
    ip_header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        total_length,
        0xC105,
        0,
        64,
        1,
        0,
        host_ip,
        guest_ip,
    )
    ip_header = ip_header[:10] + struct.pack("!H", checksum(ip_header)) + ip_header[12:]
    return ethernet_frame(ip_header + icmp, GUEST_MAC, 0x0800)


def is_arp_reply(frame: bytes, host_ip: bytes, guest_ip: bytes) -> bool:
    if len(frame) < 42 or frame[12:14] != b"\x08\x06":
        return False
    arp = frame[14:42]
    operation = struct.unpack("!H", arp[6:8])[0]
    return (
        operation == 2
        and arp[8:14] == GUEST_MAC
        and arp[14:18] == guest_ip
        and arp[18:24] == HOST_MAC
        and arp[24:28] == host_ip
    )


def is_echo_reply(frame: bytes, host_ip: bytes, guest_ip: bytes) -> bool:
    if len(frame) < 42 or frame[12:14] != b"\x08\x00":
        return False
    ip_packet = frame[14:]
    header_length = (ip_packet[0] & 0x0F) * 4
    if (
        ip_packet[0] >> 4 != 4
        or header_length < 20
        or len(ip_packet) < header_length + 8
        or ip_packet[9] != 1
        or ip_packet[12:16] != guest_ip
        or ip_packet[16:20] != host_ip
    ):
        return False
    total_length = struct.unpack("!H", ip_packet[2:4])[0]
    if total_length > len(ip_packet) or checksum(ip_packet[:header_length]) != 0:
        return False
    icmp = ip_packet[header_length:total_length]
    if len(icmp) < 8 or checksum(icmp) != 0:
        return False
    icmp_type, code, _, identifier, sequence = struct.unpack("!BBHHH", icmp[:8])
    return (
        icmp_type == 0
        and code == 0
        and identifier == IDENTIFIER
        and sequence == SEQUENCE
        and icmp[8:] == PAYLOAD
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", default="230.0.0.42")
    parser.add_argument("--port", type=int, default=12342)
    parser.add_argument("--host-ip", default="10.0.2.2")
    parser.add_argument("--guest-ip", default="10.0.2.15")
    parser.add_argument("--timeout", type=float, default=20.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    group = str(ipaddress.IPv4Address(args.group))
    host_ip = ipaddress.IPv4Address(args.host_ip).packed
    guest_ip = ipaddress.IPv4Address(args.guest_ip).packed
    host_ip_text = str(ipaddress.IPv4Address(args.host_ip))
    guest_ip_text = str(ipaddress.IPv4Address(args.guest_ip))
    if not 1 <= args.port <= 65535 or args.timeout <= 0:
        print("ICMP_PEER_FAIL invalid port or timeout", file=sys.stderr)
        return 2

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    membership = socket.inet_aton(group) + socket.inet_aton("0.0.0.0")
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    sock.settimeout(0.1)

    destination = (group, args.port)
    arp = arp_request(host_ip, guest_ip)
    echo = echo_request(host_ip, guest_ip)
    deadline = time.monotonic() + args.timeout
    next_transmit = 0.0
    arp_seen = False
    candidate_dumped = False

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_transmit:
            sock.sendto(arp, destination)
            sock.sendto(echo, destination)
            next_transmit = now + 0.25
        try:
            frame, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        if is_arp_reply(frame, host_ip, guest_ip):
            arp_seen = True
            print(f"ICMP_PEER_ARP_REPLY guest={guest_ip_text} mac=52:54:00:12:34:56")
        echo_reply = is_echo_reply(frame, host_ip, guest_ip)
        if (
            not candidate_dumped
            and not echo_reply
            and len(frame) >= 42
            and frame[6:12] == GUEST_MAC
            and frame[12:14] == b"\x08\x00"
        ):
            candidate_dumped = True
            print(f"ICMP_PEER_IP_CANDIDATE {frame.hex()}")
        if echo_reply:
            print(
                f"ICMP_PEER_ECHO_REPLY src={guest_ip_text} dst={host_ip_text} "
                f"identifier=0x{IDENTIFIER:04X} sequence={SEQUENCE} arp={'yes' if arp_seen else 'no'}"
            )
            return 0

    print("ICMP_PEER_FAIL no valid echo reply", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
