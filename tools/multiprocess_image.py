#!/usr/bin/env python3
"""Construct and independently verify Phipia's bounded multiprocess executable.

This is the third independent record of the same 256 bytes. The kernel carries
the whole file as a table in ``src/kernel/multiprocess.c``; the freestanding
Rust parser in ``src/rust/elf64.rs`` carries the instruction stream and the
structural subset. Nothing here reads either of them, so a disagreement between
any two is a build failure rather than a silent drift.
"""

from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path


FILE_BYTES = 256
ELF_HEADER_BYTES = 64
PROGRAM_HEADER_BYTES = 56
LOAD_ADDRESS = 0x0000_4000_0000_0000
CODE_OFFSET = 120
CODE_BYTES = 136
ENTRY_ADDRESS = LOAD_ADDRESS + CODE_OFFSET
PAGE_BYTES = 4096
STACK_GUARD = 0x0000_4000_0020_0000
YIELD_RESULT = 0x5341_504D
EXIT_RESULT = 0x5341_5058
YIELD_RETURN_OFFSET = 0x1F
EXIT_RETURN_OFFSET = 0x2E
FAULT_OFFSET = 0x3A
PADDING_BYTE = 0xF4

# One bounded yielding loop, written as the instructions it is rather than as
# an opaque blob. The second column is the encoding; the assembler is not a
# build dependency, so the bytes are stated and then decoded again below.
PROGRAM = (
    ("xor    %ecx, %ecx", "31c9"),
    ("mov    %rsp, %rbp", "4889e5"),
    # loop:
    ("inc    %rcx", "48ffc1"),
    ("mov    %rcx, -8(%rbp)", "48894df8"),
    ("mov    %rdi, -16(%rbp)", "48897df0"),
    ("cmp    %rdx, %rcx", "4839d1"),
    ("je     fault", "741b"),
    ("mov    $0x5341504D, %eax", "b84d504153"),
    ("mov    %rcx, %rbx", "4889cb"),
    ("int    $0x81", "cd81"),
    ("cmp    %rsi, %rcx", "4839f1"),
    ("jb     loop", "72e1"),
    ("mov    $0x53415058, %eax", "b858504153"),
    ("mov    %rcx, %rbx", "4889cb"),
    ("int    $0x81", "cd81"),
    ("ud2", "0f0b"),
    # fault:
    ("movabs $0x400000200000, %rax", "48b8000020000040 0000".replace(" ", "")),
    ("movq   $0, (%rax)", "48c700000000 00".replace(" ", "")),
    ("ud2", "0f0b"),
)

IMAGE_SHA256 = "D1FD28FE5A43252D4A5DD77BFBF0D8DDD7B07CC62BD9C44AA9E7B56476CD40F9"


def build_code() -> bytes:
    """Assemble the documented instruction table and pad with a trapping byte."""
    body = b"".join(bytes.fromhex(encoding) for _, encoding in PROGRAM)
    if len(body) > CODE_BYTES:
        raise ValueError("multiprocess program does not fit its code extent")
    code = body + bytes([PADDING_BYTE]) * (CODE_BYTES - len(body))
    if len(code) != CODE_BYTES:
        raise ValueError("multiprocess code length is not exact")
    return code


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
    payload[CODE_OFFSET:CODE_OFFSET + CODE_BYTES] = build_code()
    return bytes(payload)


def verify_payload(payload: bytes) -> None:
    """Decode every field again and reject any byte outside the contract."""
    if len(payload) != FILE_BYTES:
        raise ValueError("multiprocess ELF length is not exactly 256 bytes")
    if payload[:16] != bytes.fromhex("7f454c46020101000000000000000000"):
        raise ValueError("multiprocess ELF identification is invalid")

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
        raise ValueError("decoded multiprocess ELF header is outside the subset")

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
        raise ValueError("decoded multiprocess program header is outside the subset")
    if ENTRY_ADDRESS != LOAD_ADDRESS + CODE_OFFSET:
        raise ValueError("multiprocess entry is not the first instruction")
    if LOAD_ADDRESS % PAGE_BYTES != 0:
        raise ValueError("multiprocess load address is not page aligned")
    if ELF_HEADER_BYTES + PROGRAM_HEADER_BYTES != CODE_OFFSET:
        raise ValueError("multiprocess instructions do not follow the tables")

    code = payload[CODE_OFFSET:CODE_OFFSET + CODE_BYTES]
    if code != build_code():
        raise ValueError("multiprocess instruction bytes are invalid")
    if int.from_bytes(code[0x16:0x1A], "little") != YIELD_RESULT:
        raise ValueError("multiprocess yield result is invalid")
    if int.from_bytes(code[0x25:0x29], "little") != EXIT_RESULT:
        raise ValueError("multiprocess exit result is invalid")
    if code[YIELD_RETURN_OFFSET - 2:YIELD_RETURN_OFFSET] != b"\xCD\x81":
        raise ValueError("multiprocess yield does not return after INT 0x81")
    if code[EXIT_RETURN_OFFSET - 2:EXIT_RETURN_OFFSET] != b"\xCD\x81":
        raise ValueError("multiprocess exit does not return after INT 0x81")
    if int.from_bytes(code[FAULT_OFFSET - 8:FAULT_OFFSET], "little") != STACK_GUARD:
        raise ValueError("multiprocess fault does not target the stack guard")
    for offset in range(len(b"".join(bytes.fromhex(e) for _, e in PROGRAM)), CODE_BYTES):
        if code[offset] != PADDING_BYTE:
            raise ValueError("multiprocess padding is not the trapping byte")

    digest = hashlib.sha256(payload).hexdigest().upper()
    if digest != IMAGE_SHA256:
        raise ValueError("multiprocess ELF SHA-256 is invalid")


def kernel_table(source: Path) -> bytes:
    """Extract the byte table the kernel carries, without executing anything."""
    text = source.read_text(encoding="utf-8")
    match = re.search(
        r"static const uint8_t multiprocess_image\[[^\]]*\] = \{(.*?)\n\};",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ValueError("the kernel multiprocess image table was not found")
    values = re.findall(r"0[xX]([0-9A-Fa-f]{2})", match.group(1))
    return bytes(int(value, 16) for value in values)
