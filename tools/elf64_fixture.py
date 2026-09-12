#!/usr/bin/env python3
"""Construct and independently verify Phipia's exact ELF64 proof payload."""

from __future__ import annotations

import hashlib
import struct


FILE_BYTES = 128
ELF_HEADER_BYTES = 64
PROGRAM_HEADER_BYTES = 56
LOAD_ADDRESS = 0x0000_4000_0000_0000
ENTRY_ADDRESS = LOAD_ADDRESS + 120
PAGE_BYTES = 4096
RESULT = 0x5341_5037
PROOF_VECTOR = 0x81
CODE = bytes.fromhex("b837504153cd81f4")
PAYLOAD_SHA256 = "C923A94F08DF64523D3DB701E4F9FC5FF5B51DFC21447E1DC57586D40D42B8A9"
EXPECTED_HEX = (
    "7f454c46020101000000000000000000"
    "02003e00010000007800000000400000"
    "40000000000000000000000000000000"
    "00000000400038000100000000000000"
    "01000000050000000000000000000000"
    "00000000004000000000000000000000"
    "80000000000000008000000000000000"
    "0010000000000000b837504153cd81f4"
)
EXPECTED_BYTES = bytes.fromhex(EXPECTED_HEX)


def build_payload() -> bytes:
    """Build the exact ELF with scalar little-endian encoders, not a linker."""
    payload = bytearray(FILE_BYTES)
    payload[:16] = bytes.fromhex("7f454c46020101000000000000000000")
    struct.pack_into(
        "<HHIQQQIHHHHHH",
        payload,
        16,
        2,
        62,
        1,
        ENTRY_ADDRESS,
        ELF_HEADER_BYTES,
        0,
        0,
        ELF_HEADER_BYTES,
        PROGRAM_HEADER_BYTES,
        1,
        0,
        0,
        0,
    )
    struct.pack_into(
        "<IIQQQQQQ",
        payload,
        ELF_HEADER_BYTES,
        1,
        5,
        0,
        LOAD_ADDRESS,
        0,
        FILE_BYTES,
        FILE_BYTES,
        PAGE_BYTES,
    )
    payload[120:128] = CODE
    built = bytes(payload)
    if built != EXPECTED_BYTES:
        raise ValueError("constructed ELF differs from the independent byte record")
    return built


def verify_payload(payload: bytes) -> None:
    """Decode every reopened field and reject any non-contract byte."""
    if len(payload) != FILE_BYTES:
        raise ValueError("ELF length is not exactly 128 bytes")
    if payload != EXPECTED_BYTES:
        raise ValueError("ELF differs from the complete expected byte record")
    if payload[:16] != bytes.fromhex("7f454c46020101000000000000000000"):
        raise ValueError("ELF identification is invalid")

    header = struct.unpack_from("<HHIQQQIHHHHHH", payload, 16)
    expected_header = (
        2,
        62,
        1,
        ENTRY_ADDRESS,
        ELF_HEADER_BYTES,
        0,
        0,
        ELF_HEADER_BYTES,
        PROGRAM_HEADER_BYTES,
        1,
        0,
        0,
        0,
    )
    if header != expected_header:
        raise ValueError("decoded ELF header is outside the exact subset")

    program = struct.unpack_from("<IIQQQQQQ", payload, ELF_HEADER_BYTES)
    expected_program = (
        1,
        5,
        0,
        LOAD_ADDRESS,
        0,
        FILE_BYTES,
        FILE_BYTES,
        PAGE_BYTES,
    )
    if program != expected_program:
        raise ValueError("decoded program header is outside the exact subset")
    if ENTRY_ADDRESS < LOAD_ADDRESS or ENTRY_ADDRESS >= LOAD_ADDRESS + FILE_BYTES:
        raise ValueError("entry is outside the file-backed executable extent")
    offset = program[2]
    alignment = program[7]
    if (LOAD_ADDRESS % PAGE_BYTES != 0 or
            LOAD_ADDRESS % alignment != offset % alignment):
        raise ValueError("load-address alignment or congruence is invalid")
    if payload[120:128] != CODE:
        raise ValueError("proof instruction bytes are invalid")
    if int.from_bytes(CODE[1:5], "little") != RESULT or CODE[6] != PROOF_VECTOR:
        raise ValueError("proof result or interrupt vector is invalid")
    digest = hashlib.sha256(payload).hexdigest().upper()
    if digest != PAYLOAD_SHA256:
        raise ValueError("ELF SHA-256 is invalid")
