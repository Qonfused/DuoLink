#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Read Intel Mach-O images and inspect code for build tools and tests."""

from dataclasses import dataclass
import re
import struct
import subprocess
import uuid

X86_64 = 0x01000007

MACHO_64 = 0xFEEDFACF


class Unsupported(ValueError):
    """The current binary does not match a supported instruction layout."""


def require(condition, message):
    if not condition:
        raise Unsupported(message)


def unique(items, label):
    items = list(items)
    require(len(items) == 1, f"expected one {label}, found {len(items)}")
    return items[0]


def cmd(*args):
    return subprocess.run(
        args, check=True, capture_output=True, text=True
    ).stdout


@dataclass(frozen=True)
class Section:
    segment: str
    name: str
    address: int
    size: int
    offset: int


class MachO:
    def __init__(self, data, offset, size):
        self.offset, self.size = offset, size
        require(
            0 <= offset <= offset + size <= len(data) and size >= 32,
            "invalid Mach-O bounds",
        )
        magic, cpu, _, _, count, length, _, _ = struct.unpack_from(
            "<8I", data, offset
        )
        require(magic == MACHO_64 and cpu == X86_64, "expected an Intel Mach-O")
        self.command_end = 32 + length
        require(
            self.command_end <= size and count <= length // 8,
            "invalid load commands",
        )
        self.sections, self.segments, self.uuid, self.encrypted = (
            [],
            {},
            None,
            False,
        )
        cursor = offset + 32
        end = offset + self.command_end
        for _ in range(count):
            require(cursor + 8 <= end, "truncated load command")
            command, length = struct.unpack_from("<II", data, cursor)
            require(8 <= length <= end - cursor, "invalid load command")
            if command == 0x1B:
                require(
                    length == 24 and self.uuid is None, "invalid binary UUID"
                )
                self.uuid = str(uuid.UUID(bytes=data[cursor + 8 : cursor + 24]))
            if command in (0x21, 0x2C):
                require(length >= 20, "truncated encryption command")
                self.encrypted |= bool(
                    struct.unpack_from("<I", data, cursor + 16)[0]
                )
            if command == 0x19:
                require(length >= 72, "truncated segment")
                (
                    name,
                    address,
                    span,
                    file_offset,
                    file_size,
                    _,
                    prot,
                    sections,
                    flags,
                ) = struct.unpack_from("<16sQQQQiiII", data, cursor + 8)
                name = name.rstrip(b"\0").decode("ascii")
                require(
                    72 + 80 * sections <= length and name not in self.segments,
                    "invalid segment",
                )
                self.segments[name] = (
                    address,
                    span,
                    file_offset,
                    file_size,
                    prot,
                    flags,
                )
                for i in range(sections):
                    sec, seg, va, size, off = struct.unpack_from(
                        "<16s16sQQI", data, cursor + 72 + 80 * i
                    )
                    self.sections.append(
                        Section(
                            seg.rstrip(b"\0").decode("ascii"),
                            sec.rstrip(b"\0").decode("ascii"),
                            va,
                            size,
                            off,
                        )
                    )
            cursor += length
        require(cursor == end, "invalid command area")

    def section(self, name):
        return unique(
            (
                s
                for s in self.sections
                if s.name == name and s.segment == "__TEXT"
            ),
            name,
        )


def intel_slice(data):
    require(len(data) >= 8, "truncated binary")
    magic, count = struct.unpack_from(">II", data)
    if magic != 0xCAFEBABE:
        return MachO(data, 0, len(data))
    require(count <= (len(data) - 8) // 20, "invalid universal binary")
    slices = []
    for i in range(count):
        cpu, _, off, size, _ = struct.unpack_from(">IIIII", data, 8 + i * 20)
        if cpu == X86_64:
            slices.append((off, size))
    return MachO(data, *unique(slices, "Intel slice"))


def object_code(code, address=0):
    """Give objdump a text section while keeping its original virtual addresses."""
    off = 184
    header = struct.pack("<8I", MACHO_64, X86_64, 3, 1, 1, 152, 0, 0)
    segment = struct.pack(
        "<II16sQQQQiiII",
        0x19,
        152,
        b"__TEXT",
        address,
        len(code),
        off,
        len(code),
        7,
        5,
        1,
        0,
    )
    section = struct.pack(
        "<16s16sQQIIIIIIII",
        b"__text",
        b"__TEXT",
        address,
        len(code),
        off,
        0,
        0,
        0,
        0x80000400,
        0,
        0,
        0,
    )
    return header + segment + section + code


INSTRUCTION = re.compile(
    r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}[ \t]+)+)([a-z][a-z0-9]*)\s*(.*?)\s*$"
)


@dataclass(frozen=True)
class Instruction:
    address: int
    data: bytes
    mnemonic: str
    operands: str
    comment: str

    @property
    def end(self):
        return self.address + len(self.data)

    @property
    def target(self):
        match = re.match(r"0x([0-9a-f]+)(?:\s|$)", self.operands)
        return int(match[1], 16) if match else None

    @property
    def rip_target(self):
        match = re.match(r"(-?(?:0x[0-9a-f]+|\d+))\(%rip\),", self.operands)
        return self.end + int(match[1], 0) if match else None


def instructions(text):
    for line in text.splitlines():
        match = INSTRUCTION.match(line)
        if match:
            address, data, mnemonic, body = match.groups()
            operands, _, comment = body.partition("##")
            yield Instruction(
                int(address, 16),
                bytes.fromhex(data),
                mnemonic,
                operands.strip(),
                comment.strip(),
            )
