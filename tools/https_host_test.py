#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise Phipia's bounded HTTPS client against offline TLS peers."""

from __future__ import annotations

import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "fixtures" / "tls"
HOSTNAME = "repo.phipia.test"
BODY = b"hello from the Phipia HTTPS peer\n"

HTTPS_OK = 0
HTTPS_TIMEOUT = 8
HTTPS_CANCELED = 9
HTTPS_RESET = 10
HTTPS_TRUNCATED = 11
HTTPS_HOSTNAME = 12
HTTPS_CERTIFICATE_TIME = 13
HTTPS_AUTHENTICATION = 14
HTTPS_HTTP_VERSION = 17
HTTPS_HTTP_STATUS = 18
HTTPS_HTTP_HEADERS = 19
HTTPS_CONTENT_LENGTH_REQUIRED = 20
HTTPS_CONTENT_TOO_LARGE = 21
HTTPS_BODY_TRUNCATED = 22
HTTPS_BODY_EXTRA = 23
HTTPS_BODY_WRITE = 25


RESPONSES = {
    "success": (
        b"HTTP/1.1 200 OK\r\nContent-Length: "
        + str(len(BODY)).encode("ascii")
        + b"\r\nConnection: close\r\n\r\n"
        + BODY
    ),
    "http-version": b"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
    "http-status": b"HTTP/1.1 404 Missing\r\nContent-Length: 0\r\n\r\n",
    "duplicate-length": (
        b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        b"Content-Length: 2\r\n\r\nno"
    ),
    "transfer-encoding": (
        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
    ),
    "content-encoding": (
        b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        b"Content-Encoding: gzip\r\n\r\nno"
    ),
    "bad-line-ending": b"HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n",
    "missing-length": b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n",
    "too-large": b"HTTP/1.1 200 OK\r\nContent-Length: 129\r\n\r\n",
    "body-truncated": b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nshort",
    "body-extra": b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nnoextra",
}
RESPONSES["stream-success"] = RESPONSES["success"]
RESPONSES["stream-refusal"] = RESPONSES["success"]


class Peer:
    def __init__(self, certificate: str | None, mode: str) -> None:
        self.certificate = certificate
        self.mode = mode
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.error: Exception | None = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def finish(self) -> None:
        self.thread.join(6.0)
        self.listener.close()
        if self.thread.is_alive():
            raise RuntimeError(f"offline HTTPS peer did not terminate: {self.mode}")
        if self.error is not None:
            raise self.error

    def _context(self) -> ssl.SSLContext:
        assert self.certificate is not None
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers(
            "ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256"
        )
        context.load_cert_chain(
            FIXTURE / f"{self.certificate}.pem",
            FIXTURE / f"{self.certificate}-key.pem",
        )
        return context

    def _serve(self) -> None:
        strict = self.mode in ("success", "stream-success")
        try:
            connection, _ = self.listener.accept()
            with connection:
                if self.mode == "timeout":
                    time.sleep(0.8)
                    return
                if self.mode == "reset":
                    connection.setsockopt(
                        socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack(
                            "hh" if sys.platform == "win32" else "ii", 1, 0
                        ),
                    )
                    return
                if self.mode == "truncated-handshake":
                    connection.sendall(b"\x16\x03\x03")
                    connection.shutdown(socket.SHUT_WR)
                    time.sleep(0.1)
                    return
                context = self._context()
                try:
                    secure = context.wrap_socket(connection, server_side=True)
                except (ConnectionError, OSError, ssl.SSLError):
                    return
                with secure:
                    if self.mode in ("cancel", "expired-operation"):
                        try:
                            secure.recv(1)
                        except (ConnectionError, OSError, ssl.SSLError):
                            pass
                        return
                    request = bytearray()
                    while b"\r\n\r\n" not in request and len(request) < 2048:
                        block = secure.recv(512)
                        if not block:
                            return
                        request.extend(block)
                    expected = (
                        f"GET /artifact.bin HTTP/1.1\r\n"
                        f"Host: {HOSTNAME}:{self.port}\r\n"
                        "Accept: application/octet-stream\r\n"
                        "Accept-Encoding: identity\r\n"
                        "Connection: close\r\n\r\n"
                    ).encode("ascii")
                    if self.mode in ("success", "stream-success",
                                     "stream-refusal") and bytes(request) != expected:
                        raise RuntimeError("canonical HTTPS request bytes changed")
                    response = RESPONSES[self.mode]
                    secure.sendall(response)
                    if self.mode == "body-truncated":
                        return
                    try:
                        raw = secure.unwrap()
                        raw.close()
                    except (ConnectionError, OSError, ssl.SSLError):
                        if strict:
                            raise
        except Exception as error:  # surfaced by finish()
            self.error = error


def run_case(binary: Path, name: str, certificate: str | None,
             hostname: str, mode: str, expected: int) -> None:
    peer = Peer(certificate, mode)
    peer.start()
    command = [str(binary), str(peer.port), hostname, mode, str(expected)]
    result = subprocess.run(command, text=True, capture_output=True, timeout=7.0)
    try:
        peer.finish()
    finally:
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"{name}: client exited {result.returncode}")
    if "HTTPS HANDLES clean" not in result.stdout:
        raise RuntimeError(f"{name}: missing clean-handle proof")
    print(f"HTTPS case passed: {name}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: https_host_test.py CLIENT-BINARY")
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise SystemExit(f"missing HTTPS host client: {binary}")
    cases = [
        ("trusted-download", "valid", HOSTNAME, "success", HTTPS_OK),
        ("trusted-stream", "valid", HOSTNAME, "stream-success", HTTPS_OK),
        ("stream-sink-refusal", "valid", HOSTNAME,
         "stream-refusal", HTTPS_BODY_WRITE),
        ("hostname-mismatch", "valid", "wrong.phipia.test", "success", HTTPS_HOSTNAME),
        ("expired", "expired", HOSTNAME, "success", HTTPS_CERTIFICATE_TIME),
        ("not-yet-valid", "future", HOSTNAME, "success", HTTPS_CERTIFICATE_TIME),
        ("untrusted-root", "untrusted", HOSTNAME, "success", HTTPS_AUTHENTICATION),
        ("deadline", None, HOSTNAME, "timeout", HTTPS_TIMEOUT),
        ("expired-operation-deadline", "valid", HOSTNAME,
         "expired-operation", HTTPS_TIMEOUT),
        ("cancellation", "valid", HOSTNAME, "cancel", HTTPS_CANCELED),
        ("connection-reset", None, HOSTNAME, "reset", HTTPS_RESET),
        ("truncated-handshake", None, HOSTNAME, "truncated-handshake", HTTPS_TRUNCATED),
        ("http-version", "valid", HOSTNAME, "http-version", HTTPS_HTTP_VERSION),
        ("http-status", "valid", HOSTNAME, "http-status", HTTPS_HTTP_STATUS),
        ("duplicate-length", "valid", HOSTNAME, "duplicate-length", HTTPS_HTTP_HEADERS),
        ("transfer-encoding", "valid", HOSTNAME, "transfer-encoding", HTTPS_HTTP_HEADERS),
        ("content-encoding", "valid", HOSTNAME, "content-encoding", HTTPS_HTTP_HEADERS),
        ("bad-line-ending", "valid", HOSTNAME, "bad-line-ending", HTTPS_HTTP_HEADERS),
        ("missing-length", "valid", HOSTNAME, "missing-length", HTTPS_CONTENT_LENGTH_REQUIRED),
        ("too-large", "valid", HOSTNAME, "too-large", HTTPS_CONTENT_TOO_LARGE),
        ("body-truncated", "valid", HOSTNAME, "body-truncated", HTTPS_BODY_TRUNCATED),
        ("body-extra", "valid", HOSTNAME, "body-extra", HTTPS_BODY_EXTRA),
    ]
    for case in cases:
        run_case(binary, *case)
    print("Phipia HTTPS host tests passed: authenticated bounded download and named refusals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
