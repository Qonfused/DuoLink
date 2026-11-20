#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Derive a checked input ABI from official binaries and matching debug symbols."""

from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
import re
import struct
import subprocess
import sys

TOUCHSCREEN = "VoodooI2CTouchscreenHIDEventDriver"
TRANSDUCER = "VoodooI2CDigitiserTransducer"
PREFIX = "__ZN34" + TOUCHSCREEN
HID_METHODS = {
    "framebuffer": PREFIX + "14getFramebufferEv",
    "stylus": PREFIX + "11checkStylusEy24VoodooI2CMultitouchEvent",
    "fingerLift": PREFIX + "10fingerLiftEv",
    "forwardReport": PREFIX + "13forwardReportE24VoodooI2CMultitouchEventy",
    "start": PREFIX + "11handleStartEP9IOService",
    "stop": PREFIX + "10handleStopEP9IOService",
    "precisionTouchpadMode": (
        "__ZN40VoodooI2CPrecisionTouchpadHIDEventDriver"
        "26enterPrecisionTouchpadModeEv"
    ),
}
CORE_METHODS = {
    "interruptType": "__ZN18VoodooI2CDeviceNub16getInterruptTypeEiPi",
    "gpioLookup": "__ZN18VoodooI2CDeviceNub17getGPIOControllerEv",
}
VTABLE = "__ZTV34" + TOUCHSCREEN


@dataclass(frozen=True)
class Symbol:
    address: int
    entry: bytes


@dataclass
class Image:
    data: bytes
    uuid: bytes
    symbols: dict
    segments: list

    def bytes_at(self, address, size):
        locations = [
            offset + address - base
            for base, _, offset, length in self.segments
            if base <= address and address - base + size <= length
        ]
        if len(locations) != 1:
            raise ValueError("Address is outside uniquely mapped file data")
        offset = locations[0]
        return self.data[offset : offset + size]


def image(data):
    if len(data) < 32:
        raise ValueError("Truncated Mach-O header")
    magic, cpu, _, kind, count, length, _, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, kind) != (0xFEEDFACF, 0x01000007, 0xB):
        raise ValueError("Expected an Intel kext")
    end = 32 + length
    if end > len(data) or count > length // 8:
        raise ValueError("Truncated load commands")
    uuid, table, segments = None, None, []
    offset = 32
    for _ in range(count):
        if end - offset < 8:
            raise ValueError("Truncated load command header")
        command, size = struct.unpack_from("<2I", data, offset)
        if size < 8 or size > end - offset:
            raise ValueError("Invalid load command length")
        if command == 0x1B:
            if size != 24 or uuid is not None:
                raise ValueError("Invalid or duplicate UUID command")
            uuid = data[offset + 8 : offset + 24]
        elif command == 2:
            if size != 24 or table is not None:
                raise ValueError("Invalid or duplicate symbol table")
            table = struct.unpack_from("<4I", data, offset + 8)
        elif command == 0x19:
            if size < 72:
                raise ValueError("Truncated segment command")
            segment = struct.unpack_from("<4Q", data, offset + 24)
            if segment[2] + segment[3] > len(data):
                raise ValueError("Segment exceeds file size")
            segments.append(segment)
        offset += size
    if offset != end or uuid is None or table is None:
        raise ValueError("Missing Mach-O metadata")

    symoff, count, stroff, strsize = table
    if symoff + count * 16 > len(data) or stroff + strsize > len(data):
        raise ValueError("Truncated symbol table")
    result = Image(data, uuid, {}, segments)
    for index in range(count):
        string, kind, section, _, address = struct.unpack_from(
            "<IBBHQ", data, symoff + index * 16
        )
        if kind & 0xE0 or (kind & 0xE) != 0xE or not (kind & 1) or not section:
            continue
        if string >= strsize:
            raise ValueError("Symbol name exceeds string table")
        stop = data.find(b"\0", stroff + string, stroff + strsize)
        if stop < 0:
            raise ValueError("Unterminated symbol name")
        name = data[stroff + string : stop].decode("utf-8")
        try:
            entry = result.bytes_at(address, 8)
        except ValueError:
            continue
        symbol = Symbol(address, entry)
        if name in result.symbols and result.symbols[name] != symbol:
            raise ValueError("Ambiguous symbol: " + name)
        result.symbols[name] = symbol
    return result


@dataclass
class DebugEntry:
    tag: str
    attributes: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    @property
    def name(self):
        value = self.attributes.get("DW_AT_name", "")
        match = re.fullmatch(r'\("(.*)"\)', value)
        return match[1] if match else None

    def integer(self, attribute):
        value = self.attributes.get(attribute, "")
        match = re.fullmatch(r"\((?:DW_OP_constu )?(0x[0-9a-f]+)\)", value)
        if not match:
            raise ValueError(f"Missing integer {self.name}.{attribute}")
        return int(match[1], 16)


def parse_debug_entries(text):
    """Keep DIE nesting so member names are resolved within their type."""
    roots, stack = [], []
    current = None
    for line in text.splitlines():
        match = re.match(r"^0x[0-9a-f]+:( +)(DW_TAG_\w+|NULL)\b", line)
        if match:
            depth, tag = len(match[1]), match[2]
            while stack and stack[-1][0] >= depth:
                stack.pop()
            current = None
            if tag == "NULL":
                continue
            current = DebugEntry(tag)
            if stack:
                stack[-1][1].children.append(current)
            else:
                roots.append(current)
            stack.append((depth, current))
        elif current:
            attribute = re.match(r"\s+(DW_AT_\w+)\s+(.*)", line)
            if attribute:
                current.attributes[attribute[1]] = attribute[2]
    return roots


class DebugInfo:
    def __init__(self, path):
        self.path = str(path)

    @lru_cache(maxsize=None)
    def types(self, name):
        output = subprocess.check_output(
            [
                "/usr/bin/dwarfdump",
                "--show-children",
                "--name=" + name,
                self.path,
            ],
            text=True,
        )
        entries = []
        for entry in parse_debug_entries(output):
            if entry.name != name:
                continue
            seen = set()
            while entry.tag == "DW_TAG_typedef":
                reference = re.match(
                    r"\((0x[0-9a-f]+)", entry.attributes.get("DW_AT_type", "")
                )
                if not reference or reference[1] in seen:
                    raise ValueError("Invalid typedef chain: " + name)
                seen.add(reference[1])
                entry = self.type_at(reference[1])
            if entry.tag in ("DW_TAG_class_type", "DW_TAG_structure_type"):
                if "DW_AT_byte_size" in entry.attributes:
                    entries.append(entry)
        if not entries:
            raise ValueError("Missing debug type: " + name)
        return entries

    @lru_cache(maxsize=None)
    def type_at(self, offset):
        output = subprocess.check_output(
            [
                "/usr/bin/dwarfdump",
                "--debug-info=" + offset,
                "--show-children",
                self.path,
            ],
            text=True,
        )
        entries = parse_debug_entries(output)
        if len(entries) != 1:
            raise ValueError("Ambiguous debug type at " + offset)
        return entries[0]

    @staticmethod
    def unique(values, description):
        values = set(values)
        if len(values) != 1:
            raise ValueError(f"Ambiguous {description}: {values}")
        return values.pop()

    def size(self, name):
        return self.unique(
            (entry.integer("DW_AT_byte_size") for entry in self.types(name)),
            name + " size",
        )

    def child_integer(self, owner, name, tag, attribute):
        return self.unique(
            (
                child.integer(attribute)
                for entry in self.types(owner)
                for child in entry.children
                if child.name == name
                and child.tag == tag
                and attribute in child.attributes
            ),
            owner + "." + name,
        )

    def member(self, owner, name):
        return self.child_integer(
            owner, name, "DW_TAG_member", "DW_AT_data_member_location"
        )

    def slot(self, owner, name):
        return self.child_integer(
            owner, name, "DW_TAG_subprogram", "DW_AT_vtable_elem_location"
        )


def verify_dsym(uuid, dsym):
    output = subprocess.check_output(
        ["/usr/bin/dwarfdump", "--uuid", str(dsym)], text=True
    )
    if uuid.hex() not in output.lower().replace("-", ""):
        raise ValueError("dSYM UUID does not match the stock executable")


def verify_call_sites(binary):
    output = subprocess.check_output(
        ["/usr/bin/otool", "-tvV", str(binary)], text=True
    )
    methods = HID_METHODS
    ptp = "__ZN40VoodooI2CPrecisionTouchpadHIDEventDriver"
    wanted = {
        (methods["forwardReport"], methods["framebuffer"]),
        (methods["forwardReport"], methods["stylus"]),
        (methods["start"], methods["framebuffer"]),
        (methods["start"], methods["fingerLift"]),
        (ptp + "11handleStartEP9IOService", methods["precisionTouchpadMode"]),
        (
            ptp + "13setPowerStateEmP9IOService",
            methods["precisionTouchpadMode"],
        ),
    }
    # Some optimized call sites load a method address before calling it.
    found, owner = set(), ""
    for line in output.splitlines():
        if line.endswith(":") and not line.startswith((" ", "\t")):
            owner = line[:-1]
        if "callq" in line or "leaq" in line:
            found.update(
                pair for pair in wanted if owner == pair[0] and pair[1] in line
            )
    if found != wanted:
        raise ValueError("A native interception point was inlined or removed")


def profile(core_image, hid_image, dsym):
    debug = DebugInfo(dsym)
    current = debug.member("TimeTrackedValue", "current")
    value = current + debug.member("ValueWithTime", "value")
    timestamp = current + debug.member("ValueWithTime", "timestamp")
    coordinates = debug.member(TRANSDUCER, "coordinates")
    coordinate_type = "DigitiserTransducerCoordinates"
    x = coordinates + debug.member(coordinate_type, "x")
    y = coordinates + debug.member(coordinate_type, "y")
    z = coordinates + debug.member(coordinate_type, "z")
    if (
        debug.size("TimeTrackedValue"),
        debug.size("ValueWithTime"),
        current,
        value,
        timestamp,
    ) != (32, 16, 0, 0, 8):
        raise ValueError("Unsupported tracked-value layout")

    result = {
        "hid_uuid": hid_image.uuid,
        "core_uuid": core_image.uuid,
        "dispatch_slot": debug.slot(
            "IOHIDEventService", "dispatchDigitizerEventWithTiltOrientation"
        ),
        "start_slot": debug.slot(TOUCHSCREEN, "handleStart"),
        "stop_slot": debug.slot(TOUCHSCREEN, "handleStop"),
        "report_slot": debug.slot(TOUCHSCREEN, "forwardReport"),
        "framebuffer_offset": debug.member(TOUCHSCREEN, "active_framebuffer"),
        "driver_size": debug.size(TOUCHSCREEN),
        "stylus_size": debug.size("VoodooI2CDigitiserStylus"),
        "x_timestamp": x + timestamp,
        "y_timestamp": y + timestamp,
        "z_value": z + value,
        "pressure_value": debug.member(TRANSDUCER, "tip_pressure") + value,
        "z_maximum": debug.member(TRANSDUCER, "logical_max_z"),
        "pressure_maximum": debug.member(TRANSDUCER, "pressure_physical_max"),
        "in_range": debug.member(TRANSDUCER, "in_range"),
    }
    last_slot = max(
        result[key]
        for key in ("dispatch_slot", "start_slot", "stop_slot", "report_slot")
    )
    # Two ABI entries precede slot zero. Include the final method pointer.
    result["vtable_size"] = 8 * (2 + last_slot + 1)
    if result["framebuffer_offset"] + 8 > result["driver_size"]:
        raise ValueError("Framebuffer field exceeds native class size")
    for name, size in [
        ("x_timestamp", 8),
        ("y_timestamp", 8),
        ("z_value", 2),
        ("pressure_value", 2),
        ("z_maximum", 4),
        ("pressure_maximum", 4),
        ("in_range", 1),
    ]:
        if result[name] + size > result["stylus_size"]:
            raise ValueError("Pen field exceeds native class size: " + name)

    table = hid_image.symbols[VTABLE].address
    hid_image.bytes_at(table, result["vtable_size"])
    for field, method in [
        ("start_slot", "start"),
        ("stop_slot", "stop"),
        ("report_slot", "forwardReport"),
    ]:
        address = table + (result[field] + 2) * 8
        entry = struct.unpack("<Q", hid_image.bytes_at(address, 8))[0]
        if entry != hid_image.symbols[HID_METHODS[method]].address:
            raise ValueError(
                "Debug virtual slots do not match the stock binary"
            )
    return result


def byte_array(value):
    return "{" + ", ".join(f"0x{byte:02x}" for byte in value) + "}"


def generate(core, hid, dsym, output):
    core_image = image(Path(core).read_bytes())
    hid_image = image(Path(hid).read_bytes())
    verify_dsym(hid_image.uuid, dsym)

    for binary, names in (
        (core_image, CORE_METHODS.values()),
        (hid_image, [*HID_METHODS.values(), VTABLE]),
    ):
        for name in names:
            if name not in binary.symbols:
                raise ValueError("Missing stock symbol: " + name)
    verify_call_sites(hid)
    values = profile(core_image, hid_image, dsym)

    lines = [
        "// Generated from official 2.9.1 RELEASE binaries and matching debug data.",
        "#pragma once",
        "#include <stdint.h>",
        "namespace duolink { namespace input { namespace profile {",
        "struct Method { const char *symbol; uint8_t entry[8]; };",
    ]
    for key, value in values.items():
        if isinstance(value, bytes):
            lines.append(f"constexpr uint8_t {key}[16] = {byte_array(value)};")
        else:
            lines.append(f"constexpr unsigned {key} = {value};")
    for binary, methods in (
        (core_image, CORE_METHODS),
        (hid_image, HID_METHODS),
    ):
        for name, symbol in methods.items():
            entry = byte_array(binary.symbols[symbol].entry)
            lines.append(f'constexpr Method {name} {{"{symbol}", {entry}}};')
    lines.append(f'constexpr const char *vtable_symbol = "{VTABLE}";')
    lines.append("} } } // namespace duolink::input::profile")

    Path(output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: generate_hid_profile.py CORE HID DSYM OUTPUT")
    generate(*sys.argv[1:])
