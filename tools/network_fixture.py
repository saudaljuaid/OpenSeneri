#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic, offline Ethernet peer for Phipia's QEMU socket backend."""

from __future__ import annotations

import argparse
import ipaddress
import os
import socket
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

GROUP = "230.42.0.1"
PORT = 18421
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("525400654321")
BROADCAST_MAC = b"\xff" * 6
GUEST_IP = ipaddress.IPv4Address("10.0.2.15").packed
GATEWAY_IP = ipaddress.IPv4Address("10.0.2.2").packed
DNS_IP = ipaddress.IPv4Address("10.0.2.3").packed
HTTP_IP = ipaddress.IPv4Address("10.0.2.20").packed
BROADCAST_IP = b"\xff" * 4
WELCOME = b"hello from the Phipia network\n"

# Phipia announces a port it is listening on, or one it deliberately is not,
# over UDP; this peer then opens a TCP connection *to* the guest. The guest is
# the server in those two scenarios, which is the only way to exercise a
# passive open from outside.
KNOCK_PORT = 4243
KNOCK_MAGIC = b"PHIP"
LISTEN_REQUEST = b"PHIPIA LISTEN\n"
REFUSAL_NOTICE = b"REFUSED"
CLIENT_PORT = 50100
CLIENT_ISN = 0x71000000


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet(destination: bytes, source: bytes, kind: int, payload: bytes) -> bytes:
    return destination + source + struct.pack("!H", kind) + payload


def ipv4(source: bytes, destination: bytes, protocol: int, payload: bytes,
         identity: int = 1, ttl: int = 64, bad_checksum: bool = False) -> bytes:
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload),
                         identity & 0xFFFF, 0x4000, ttl, protocol, 0,
                         source, destination)
    value = checksum(header)
    if bad_checksum:
        value ^= 0x0100
    header = header[:10] + struct.pack("!H", value) + header[12:]
    return header + payload


def udp(source_ip: bytes, destination_ip: bytes, source_port: int,
        destination_port: int, payload: bytes, zero_checksum: bool = False) -> bytes:
    length = 8 + len(payload)
    header = struct.pack("!HHHH", source_port, destination_port, length, 0)
    pseudo = source_ip + destination_ip + struct.pack("!BBH", 0, 17, length)
    value = 0 if zero_checksum else checksum(pseudo + header + payload)
    if value == 0 and not zero_checksum:
        value = 0xFFFF
    return struct.pack("!HHHH", source_port, destination_port, length, value) + payload


def tcp(source_ip: bytes, destination_ip: bytes, source_port: int,
        destination_port: int, sequence: int, acknowledgement: int,
        flags: int, payload: bytes = b"", options: bytes = b"") -> bytes:
    if len(options) % 4:
        options += b"\x01" * (-len(options) % 4)
    offset = (20 + len(options)) // 4
    header = struct.pack("!HHIIBBHHH", source_port, destination_port,
                         sequence & 0xFFFFFFFF, acknowledgement & 0xFFFFFFFF,
                         offset << 4, flags, 8192, 0, 0) + options
    length = len(header) + len(payload)
    pseudo = source_ip + destination_ip + struct.pack("!BBH", 0, 6, length)
    value = checksum(pseudo + header + payload)
    return header[:16] + struct.pack("!H", value) + header[18:] + payload


def arp_reply(target_mac: bytes, target_ip: bytes, claimed_ip: bytes,
              claimed_mac: bytes = PEER_MAC) -> bytes:
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
    payload += claimed_mac + claimed_ip + target_mac + target_ip
    return ethernet(target_mac, claimed_mac, 0x0806, payload)


def parse_ipv4(frame: bytes) -> tuple[bytes, bytes, int, bytes] | None:
    if len(frame) < 34 or frame[12:14] != b"\x08\x00":
        return None
    packet = frame[14:]
    header_length = (packet[0] & 0x0F) * 4
    total = struct.unpack_from("!H", packet, 2)[0]
    if packet[0] >> 4 != 4 or header_length < 20 or total > len(packet):
        return None
    return packet[12:16], packet[16:20], packet[9], packet[header_length:total]


def dns_question(message: bytes) -> tuple[bytes, int] | None:
    if len(message) < 13:
        return None
    offset = 12
    labels: list[bytes] = []
    while offset < len(message):
        length = message[offset]
        offset += 1
        if length == 0:
            break
        if length > 63 or offset + length > len(message):
            return None
        labels.append(message[offset:offset + length])
        offset += length
    if offset + 4 > len(message):
        return None
    return b".".join(labels), offset + 4


class PcapWriter:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = path.open("wb")
        self.stream.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0,
                                      65535, 1))
        self.index = 0

    def write(self, frame: bytes) -> None:
        self.index += 1
        self.stream.write(struct.pack("<IIII", 1_700_000_000 + self.index,
                                      self.index * 1000, len(frame), len(frame)))
        self.stream.write(frame)
        self.stream.flush()

    def close(self) -> None:
        self.stream.close()


@dataclass
class ClientSession:
    """A connection this peer opens to the guest, rather than the reverse."""

    local_port: int
    remote_port: int
    send_next: int
    receive_next: int = 0
    established: bool = False
    reply_seen: bool = False
    finished: bool = False


@dataclass
class TcpPeer:
    client_port: int
    client_next: int
    server_next: int
    request_seen: bool = False


class Fixture:
    def __init__(self, group: str, port: int, peer_port: int, mode: str,
                 capture: Path, unicast: bool = False) -> None:
        self.group = group
        self.port = port
        self.peer = ("127.0.0.1", peer_port) if unicast else (group, port)
        self.mode = mode
        self.capture = PcapWriter(capture)
        self.identity = 100
        self.tcp_peers: dict[int, TcpPeer] = {}
        self.dropped_syn: set[int] = set()
        self.session: ClientSession | None = None
        self.session_count = 0
        self.knock: tuple[bytes, int] | None = None
        self.reset_seen = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
                                  socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1" if unicast else "", port))
        if not unicast:
            membership = socket.inet_aton(group) + socket.inet_aton("0.0.0.0")
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                                 membership)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)

    def close(self) -> None:
        self.capture.close()
        self.sock.close()

    def send(self, frame: bytes) -> None:
        self.capture.write(frame)
        self.sock.sendto(frame, self.peer)

    def send_ipv4(self, destination_mac: bytes, source_ip: bytes,
                  destination_ip: bytes, protocol: int, payload: bytes,
                  bad_checksum: bool = False) -> None:
        self.identity += 1
        packet = ipv4(source_ip, destination_ip, protocol, payload,
                      self.identity, bad_checksum=bad_checksum)
        self.send(ethernet(destination_mac, PEER_MAC, 0x0800, packet))

    def handle_arp(self, frame: bytes) -> None:
        if len(frame) < 42:
            return
        packet = frame[14:42]
        if packet[:8] != b"\x00\x01\x08\x00\x06\x04\x00\x01":
            return
        sender_mac, sender_ip = packet[8:14], packet[14:18]
        target_ip = packet[24:28]
        if target_ip not in (GATEWAY_IP, DNS_IP, HTTP_IP):
            return
        claimed = bytes.fromhex("525400654399") if self.mode == "arp-conflict" else PEER_MAC
        self.send(arp_reply(sender_mac, sender_ip, target_ip, claimed))
        if self.mode == "arp-conflict":
            self.send(arp_reply(sender_mac, sender_ip, target_ip, PEER_MAC))

    def dhcp_payload(self, request: bytes, message_type: int) -> bytes:
        response = bytearray(240)
        response[0:4] = b"\x02\x01\x06\x00"
        response[4:8] = request[4:8]
        response[16:20] = GUEST_IP
        response[20:24] = GATEWAY_IP
        response[28:34] = request[28:34]
        response[236:240] = b"\x63\x82\x53\x63"
        options = bytes((53, 1, message_type, 1, 4)) + b"\xff\xff\xff\x00"
        options += bytes((3, 4)) + GATEWAY_IP
        options += bytes((6, 4)) + DNS_IP
        options += bytes((54, 4)) + GATEWAY_IP
        options += bytes((51, 4)) + struct.pack("!I", 3600)
        options += bytes((58, 4)) + struct.pack("!I", 1800)
        options += bytes((59, 4)) + struct.pack("!I", 3150) + b"\xff"
        return bytes(response) + options

    @staticmethod
    def dhcp_type(payload: bytes) -> int:
        offset = 240
        while offset < len(payload):
            kind = payload[offset]
            offset += 1
            if kind == 255:
                break
            if kind == 0:
                continue
            if offset >= len(payload):
                break
            length = payload[offset]
            offset += 1
            if offset + length > len(payload):
                break
            if kind == 53 and length == 1:
                return payload[offset]
            offset += length
        return 0

    def handle_dhcp(self, source_ip: bytes, payload: bytes) -> None:
        if self.mode in ("silent", "dhcp-timeout") or len(payload) < 248:
            return
        message = self.dhcp_type(payload)
        if self.mode == "dhcp-nak" and message == 3:
            answer_type = 6
        elif message == 1:
            answer_type = 2
        elif message == 3:
            answer_type = 5
        else:
            return
        answer = self.dhcp_payload(payload, answer_type)
        datagram = udp(GATEWAY_IP, BROADCAST_IP, 67, 68, answer)
        self.send_ipv4(BROADCAST_MAC, GATEWAY_IP, BROADCAST_IP, 17, datagram)

    def handle_dns(self, source_ip: bytes, source_port: int, payload: bytes) -> None:
        if self.mode in ("silent", "dns-timeout"):
            return
        parsed = dns_question(payload)
        if parsed is None:
            return
        name, question_end = parsed
        identifier = payload[:2]
        flags = 0x8180
        answers = 1
        suffix = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + HTTP_IP
        if name == b"malformed.test":
            answer = identifier + b"\x81\x80\x00"
            datagram = udp(DNS_IP, source_ip, 53, source_port, answer)
            self.send_ipv4(GUEST_MAC, DNS_IP, source_ip, 17, datagram)
            return
        if self.mode == "dns-nxdomain" or name != b"phipia.test":
            flags, answers, suffix = 0x8183, 0, b""
        elif self.mode == "dns-truncated":
            flags, answers, suffix = 0x8380, 0, b""
        elif self.mode == "dns-cname":
            alias = b"\x05alias\x06phipia\x04test\x00"
            cname = (b"\xc0\x0c\x00\x05\x00\x01\x00\x00\x00\x3c" +
                     struct.pack("!H", len(alias)) + alias)
            address = (alias + b"\x00\x01\x00\x01\x00\x00\x00\x3c"
                       b"\x00\x04" + HTTP_IP)
            answers, suffix = 2, cname + address
        answer = identifier + struct.pack("!HHHHH", flags, 1, answers, 0, 0)
        answer += payload[12:question_end] + suffix
        datagram = udp(DNS_IP, source_ip, 53, source_port, answer,
                       zero_checksum=self.mode == "udp-zero-checksum")
        self.send_ipv4(GUEST_MAC, DNS_IP, source_ip, 17, datagram,
                       bad_checksum=self.mode == "ipv4-bad-checksum")

    def handle_icmp(self, source_ip: bytes, destination_ip: bytes,
                    payload: bytes) -> None:
        if self.mode == "silent" or len(payload) < 8 or payload[0] != 8:
            return
        answer = bytearray(payload)
        answer[0] = 0
        answer[2:4] = b"\x00\x00"
        answer[2:4] = struct.pack("!H", checksum(bytes(answer)))
        self.send_ipv4(GUEST_MAC, destination_ip, source_ip, 1, bytes(answer))

    def http_response(self, request: bytes) -> bytes:
        if self.mode == "http-chunked":
            return (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    b"Connection: close\r\n\r\n" +
                    f"{len(WELCOME):x}\r\n".encode() + WELCOME + b"\r\n0\r\n\r\n")
        if self.mode == "http-truncated":
            return b"HTTP/1.1 200 OK\r\nContent-Length: 200\r\n\r\nshort"
        if self.mode == "http-malformed":
            return b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nno"
        if self.mode == "http-redirect-loop":
            return b"HTTP/1.1 302 Found\r\nLocation: http://phipia.test/loop\r\nContent-Length: 0\r\n\r\n"
        if self.mode == "http-redirect" and b"GET /start " in request:
            return b"HTTP/1.1 302 Found\r\nLocation: http://phipia.test/welcome.txt\r\nContent-Length: 0\r\n\r\n"
        return (b"HTTP/1.1 200 OK\r\nContent-Length: " +
                str(len(WELCOME)).encode() + b"\r\nConnection: close\r\n\r\n" + WELCOME)

    def send_tcp(self, peer: TcpPeer, flags: int, payload: bytes = b"",
                 source_port: int = 80) -> None:
        segment = tcp(HTTP_IP, GUEST_IP, source_port, peer.client_port,
                      peer.server_next, peer.client_next, flags, payload)
        self.send_ipv4(GUEST_MAC, HTTP_IP, GUEST_IP, 6, segment)
        peer.server_next = (peer.server_next + len(payload) +
                            (1 if flags & 0x03 else 0)) & 0xFFFFFFFF

    def send_client(self, session: ClientSession, flags: int,
                    payload: bytes = b"") -> None:
        options = b"\x02\x04\x05\xb4" if flags & 0x02 else b""
        segment = tcp(HTTP_IP, GUEST_IP, session.local_port,
                      session.remote_port, session.send_next,
                      session.receive_next, flags, payload, options)
        self.send_ipv4(GUEST_MAC, HTTP_IP, GUEST_IP, 6, segment)
        session.send_next = (session.send_next + len(payload) +
                             (1 if flags & 0x03 else 0)) & 0xFFFFFFFF

    def handle_knock(self, source_ip: bytes, source_port: int,
                     payload: bytes) -> None:
        if self.mode not in ("tcp-listen", "tcp-refused"):
            return
        if len(payload) != 6 or payload[:4] != KNOCK_MAGIC:
            return
        if self.session is not None and not self.session.finished:
            return
        target = struct.unpack_from("!H", payload, 4)[0]
        if target == 0:
            return
        # A fresh local port per announcement, so a late segment from the
        # previous connection can never be mistaken for this one.
        self.knock = (source_ip, source_port)
        self.session = ClientSession(CLIENT_PORT + self.session_count, target,
                                     CLIENT_ISN)
        self.session_count += 1
        self.send_client(self.session, 0x02)

    def notify_knock(self, message: bytes) -> None:
        if self.knock is None:
            return
        source_ip, source_port = self.knock
        datagram = udp(HTTP_IP, source_ip, KNOCK_PORT, source_port, message)
        self.send_ipv4(GUEST_MAC, HTTP_IP, source_ip, 17, datagram)

    def handle_client(self, segment: bytes) -> None:
        session = self.session
        assert session is not None
        sequence, acknowledgement = struct.unpack_from("!II", segment, 4)
        offset = (segment[12] >> 4) * 4
        flags = segment[13]
        payload = segment[offset:]
        if flags & 0x04:
            self.reset_seen = True
            session.finished = True
            self.notify_knock(REFUSAL_NOTICE)
            return
        if not session.established:
            if flags & 0x12 != 0x12 or acknowledgement != session.send_next:
                return
            session.receive_next = (sequence + 1) & 0xFFFFFFFF
            session.established = True
            self.send_client(session, 0x10)
            self.send_client(session, 0x18, LISTEN_REQUEST)
            return
        if payload:
            if sequence != session.receive_next:
                return
            session.receive_next = (sequence + len(payload)) & 0xFFFFFFFF
            self.send_client(session, 0x10)
            if payload == WELCOME:
                session.reply_seen = True
                self.send_client(session, 0x11)
            return
        if flags & 0x01:
            session.receive_next = (sequence + 1) & 0xFFFFFFFF
            self.send_client(session, 0x10)
            session.finished = True

    def handle_tcp(self, source_ip: bytes, destination_ip: bytes,
                   segment: bytes) -> None:
        if len(segment) < 20 or destination_ip != HTTP_IP:
            return
        source_port, destination_port, sequence, acknowledgement = struct.unpack_from(
            "!HHII", segment, 0)
        offset = (segment[12] >> 4) * 4
        flags = segment[13]
        if offset < 20 or offset > len(segment):
            return
        session = self.session
        if session is not None and destination_port == session.local_port and \
                source_port == session.remote_port:
            self.handle_client(segment)
            return
        if destination_port == 81 and flags & 0x02 and not flags & 0x10:
            peer = TcpPeer(source_port, (sequence + 1) & 0xFFFFFFFF,
                           0x62000000 + source_port)
            self.send_tcp(peer, 0x14, source_port=destination_port)
            return
        if destination_port != 80:
            return
        payload = segment[offset:]
        peer = self.tcp_peers.get(source_port)
        if flags & 0x02 and not flags & 0x10:
            if self.mode == "tcp-retransmit" and source_port not in self.dropped_syn:
                self.dropped_syn.add(source_port)
                return
            peer = TcpPeer(source_port, (sequence + 1) & 0xFFFFFFFF,
                           0x62000000 + source_port)
            self.tcp_peers[source_port] = peer
            if self.mode == "tcp-reset":
                self.send_tcp(peer, 0x14)
            elif self.mode != "silent":
                self.send_tcp(peer, 0x12, b"")
            return
        if peer is None:
            return
        if payload:
            peer.client_next = (sequence + len(payload)) & 0xFFFFFFFF
            peer.request_seen = True
            response = self.http_response(payload)
            self.send_tcp(peer, 0x19, response)
        elif flags & 0x01:
            peer.client_next = (sequence + 1) & 0xFFFFFFFF
            self.send_tcp(peer, 0x10)
        _ = acknowledgement

    def handle_ipv4(self, frame: bytes) -> None:
        parsed = parse_ipv4(frame)
        if parsed is None:
            return
        source_ip, destination_ip, protocol, payload = parsed
        if protocol == 1:
            self.handle_icmp(source_ip, destination_ip, payload)
        elif protocol == 17 and len(payload) >= 8:
            source_port, destination_port, length = struct.unpack_from("!HHH", payload, 0)
            if length < 8 or length > len(payload):
                return
            body = payload[8:length]
            if source_port == 68 and destination_port == 67:
                self.handle_dhcp(source_ip, body)
            elif destination_port == 53 and destination_ip == DNS_IP:
                self.handle_dns(source_ip, source_port, body)
            elif destination_port == KNOCK_PORT:
                self.handle_knock(source_ip, source_port, body)
            elif destination_port == 4242 and destination_ip == HTTP_IP and \
                    self.mode != "silent":
                answer = udp(HTTP_IP, source_ip, 4242, source_port, body)
                self.send_ipv4(GUEST_MAC, HTTP_IP, source_ip, 17, answer)
        elif protocol == 6:
            self.handle_tcp(source_ip, destination_ip, payload)

    def handle(self, frame: bytes) -> None:
        if len(frame) < 14 or len(frame) > 1514 or frame[6:12] == PEER_MAC:
            return
        self.capture.write(frame)
        if self.mode == "malformed-flood":
            for malformed in (b"\x00" * 13,
                              ethernet(GUEST_MAC, PEER_MAC, 0x0800, b"\x45"),
                              ethernet(GUEST_MAC, PEER_MAC, 0x0806, b"\x00" * 7)):
                self.send(malformed)
        kind = struct.unpack_from("!H", frame, 12)[0]
        if kind == 0x0806:
            self.handle_arp(frame)
        elif kind == 0x0800:
            self.handle_ipv4(frame)

    def run(self, ready: Path | None) -> None:
        if ready is not None:
            ready.parent.mkdir(parents=True, exist_ok=True)
            ready.write_text(f"{self.group}:{self.port} {self.mode}\n", encoding="ascii")
        try:
            while True:
                frame, _ = self.sock.recvfrom(65535)
                self.handle(frame)
        finally:
            if ready is not None:
                ready.unlink(missing_ok=True)


def self_test() -> int:
    sample = udp(GATEWAY_IP, GUEST_IP, 67, 68, b"phipia")
    pseudo = GATEWAY_IP + GUEST_IP + struct.pack("!BBH", 0, 17, len(sample))
    assert checksum(pseudo + sample) == 0
    segment = tcp(HTTP_IP, GUEST_IP, 80, 49152, 1, 2, 0x12,
                  options=b"\x02\x04\x05\xb4")
    pseudo = HTTP_IP + GUEST_IP + struct.pack("!BBH", 0, 6, len(segment))
    assert checksum(pseudo + segment) == 0
    packet = ipv4(GATEWAY_IP, GUEST_IP, 1, b"\x00" * 8)
    assert checksum(packet[:20]) == 0
    assert len(arp_reply(GUEST_MAC, GUEST_IP, GATEWAY_IP)) == 42
    knock = KNOCK_MAGIC + struct.pack("!H", 7777)
    assert len(knock) == 6 and struct.unpack_from("!H", knock, 4)[0] == 7777
    session = ClientSession(CLIENT_PORT, 7777, CLIENT_ISN)
    segment = tcp(HTTP_IP, GUEST_IP, session.local_port, session.remote_port,
                  session.send_next, session.receive_next, 0x02,
                  options=b"\x02\x04\x05\xb4")
    pseudo = HTTP_IP + GUEST_IP + struct.pack("!BBH", 0, 6, len(segment))
    assert checksum(pseudo + segment) == 0 and segment[13] == 0x02
    print("network fixture self-test: 6/6 passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", default=GROUP)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--peer-port", type=int, default=PORT + 1)
    parser.add_argument("--unicast", action="store_true")
    parser.add_argument("--mode", default="normal", choices=(
        "normal", "silent", "dhcp-timeout", "dhcp-nak", "dns-timeout",
        "dns-nxdomain", "dns-truncated", "dns-cname", "udp-zero-checksum",
        "ipv4-bad-checksum", "arp-conflict", "tcp-reset", "tcp-retransmit",
        "http-chunked", "http-redirect",
        "http-truncated", "http-malformed", "http-redirect-loop",
        "malformed-flood", "tcp-listen", "tcp-refused"))
    parser.add_argument("--capture", type=Path, default=Path("build/network.pcap"))
    parser.add_argument("--ready", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    fixture = Fixture(args.group, args.port, args.peer_port, args.mode,
                      args.capture, args.unicast)
    try:
        fixture.run(args.ready)
    except KeyboardInterrupt:
        return 0
    finally:
        fixture.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
