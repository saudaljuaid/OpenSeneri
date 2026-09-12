#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the Phipia BearSSL wrapper against deterministic offline peers."""

from __future__ import annotations

import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "fixtures" / "tls"
HOSTNAME = "repo.phipia.test"
TLS_OK = 0
TLS_TRANSPORT = 7
TLS_HANDSHAKE = 8


class Peer:
    def __init__(self, certificate: str | None, mode: str = "tls") -> None:
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
        self.thread.join(8.0)
        self.listener.close()
        if self.thread.is_alive():
            raise RuntimeError("offline TLS peer did not terminate")
        if self.error is not None:
            raise self.error

    def _serve(self) -> None:
        try:
            connection, _ = self.listener.accept()
            with connection:
                if self.mode == "truncated":
                    connection.sendall(b"\x16\x03\x03")
                    return
                if self.mode == "timeout":
                    time.sleep(3.5)
                    return
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
                try:
                    secure = context.wrap_socket(connection, server_side=True)
                except (ConnectionError, ssl.SSLError):
                    return
                with secure:
                    request = secure.recv(128)
                    if self.certificate == "valid":
                        if request != b"GET / HTTP/1.0\r\n\r\n":
                            raise RuntimeError("authenticated request bytes changed")
                        secure.sendall(b"OK")
                    try:
                        raw = secure.unwrap()
                        raw.close()
                    except (ConnectionError, OSError, ssl.SSLError):
                        if self.certificate == "valid":
                            raise
        except Exception as error:  # surfaced by finish()
            self.error = error


def run_case(binary: Path, name: str, certificate: str | None, hostname: str,
             expected: int, mode: str = "tls") -> None:
    peer = Peer(certificate, mode)
    peer.start()
    command = [
        str(binary),
        str(FIXTURE / "anchor.txt"),
        str(peer.port),
        hostname,
        str(expected),
        "request" if expected == TLS_OK else "refusal",
    ]
    result = subprocess.run(command, text=True, capture_output=True, timeout=8.0)
    try:
        peer.finish()
    finally:
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"{name}: client exited {result.returncode}")
    print(f"TLS case passed: {name}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: tls_host_test.py CLIENT-BINARY")
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise SystemExit(f"missing TLS host client: {binary}")
    run_case(binary, "trusted", "valid", HOSTNAME, TLS_OK)
    run_case(binary, "hostname-mismatch", "valid", "wrong.phipia.test", TLS_HANDSHAKE)
    run_case(binary, "expired", "expired", HOSTNAME, TLS_HANDSHAKE)
    run_case(binary, "not-yet-valid", "future", HOSTNAME, TLS_HANDSHAKE)
    run_case(binary, "untrusted-root", "untrusted", HOSTNAME, TLS_HANDSHAKE)
    run_case(binary, "truncated-handshake", None, HOSTNAME, TLS_TRANSPORT, "truncated")
    run_case(binary, "deadline", None, HOSTNAME, TLS_TRANSPORT, "timeout")
    print("Phipia TLS host tests passed: chain, hostname, time, truncation, deadline, close")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
