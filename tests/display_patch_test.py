#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check CoreDisplay mode classifications and stack operand relocation."""

import ctypes
import hashlib
import itertools
import json
import os
import platform
from pathlib import Path
import runpy
import struct
import subprocess
import sys

workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
scratch = Path(os.environ["TEST_TMPDIR"])
header = workspace / sys.argv[1]
sys.path.insert(0, str(workspace / "tools"))
generator = runpy.run_path(str(workspace / "tools/generate_display_patch.py"))
checker = scratch / "check_display"
decoder_root = (workspace / sys.argv[2]).parent.parent.parent
objects = []
for index, source in enumerate(
    (workspace / sys.argv[2], workspace / "src/display/decode_instruction.c")
):
    obj = scratch / f"decoder-{index}.o"
    subprocess.run(
        [
            "/usr/bin/clang",
            "-std=c99",
            "-O2",
            "-fno-stack-protector",
            "-DCAPSTONE_HAS_X86=1",
            "-DCAPSTONE_DIET=1",
            "-DdecodeInstruction=duolinkDecodeX86",
            "-I",
            str(decoder_root),
            "-I",
            str(decoder_root / "include"),
            "-I",
            str(workspace / "src/display"),
            "-c",
            str(source),
            "-o",
            str(obj),
        ],
        check=True,
    )
    imports = set(
        subprocess.check_output(
            ["/usr/bin/nm", "-u", str(obj)], text=True
        ).split()
    )
    assert imports <= {
        "_memset",
        "_bzero",
        "___bzero",
        "_memcpy",
        "_memmove",
        "_duolinkDecodeX86",
    }, imports
    objects.append(str(obj))
subprocess.run(
    [
        "/usr/bin/clang++",
        "-std=c++14",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(workspace),
        "-I",
        str(header.parent),
        str(workspace / "tests/compatibility/check_display.cpp"),
        *objects,
        "-o",
        str(checker),
    ],
    check=True,
)


def frame_layout(block):
    for form in generator["LAYOUTS"]:
        pattern, operands, _, _ = generator["original_pattern"](form)
        fixed = bytearray(block[: len(pattern)])
        if len(fixed) < len(pattern) or not 0 < fixed[-1] < 128:
            continue
        fixed[-1] = pattern[-1]
        slots = {}
        for name, pos, size in operands:
            slots[name] = int.from_bytes(
                block[pos : pos + size], "little", signed=True
            )
            fixed[pos : pos + size] = bytes(size)
        if fixed == pattern:
            return form, slots
    raise AssertionError(
        "Unrecognized instruction block exported by the matcher"
    )


def assembled_replacement(form, frame, original):
    _, _, start, end = generator["original_pattern"](form)
    write_size = end - start
    fields = frame.copy()
    if "display_base" in frame:
        fields["display_flags"] = frame["display_base"] + 144
    for name, offset in frame.items():
        fields[name + "_delta"] = offset - frame["width"]
    source = generator["assembly_source"](
        form, workspace / "src/display/display_mode.s.in", "/usr/bin/clang"
    )
    prefix = generator["assemble"](
        source, fields, write_size, "/usr/bin/clang", scratch
    )
    return original[:start] + prefix + original[end:]


def check_relative_format_slot(page, site, directory, start, slots):
    # The format operand is a signed byte offset from width. Truncating an
    # out-of-range offset would make the instruction read a different local.
    _, operands, _, _ = generator["original_pattern"]("register_flags")
    offset = start + next(
        pos for name, pos, _ in operands if name == "format_check"
    )
    trial = directory / "relocated-page.bin"
    for delta in (-129, -128, 127, 128):
        modified = bytearray(page)
        struct.pack_into("<i", modified, offset, slots["width"] + delta)
        trial.write_bytes(modified)
        result = subprocess.run(
            [checker, "--page", trial, str(site)],
            capture_output=True,
            text=True,
        )
        if -128 <= delta <= 127:
            assert result.returncode == 0 and result.stdout.strip() == "applied"
        else:
            assert (
                result.returncode == 1
                and result.stdout.strip() == "page rejected"
            )


class ModeResult(ctypes.Structure):
    _fields_ = [
        ("mode", ctypes.c_void_p),
        ("saved", ctypes.c_uint64),
        ("classification", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("other", ctypes.c_uint32),
        ("padding", ctypes.c_uint32),
        ("lanes", ctypes.c_int64 * 2),
    ]


def check_display_modes(form, slots, directory, validated, vector=None):
    _, operands, start, _ = generator["original_pattern"](form)
    defines = [f"-DBODY_START={start}"]
    if vector:
        displacement = next(
            pos for name, pos, _ in operands if name == "thresholds"
        )
        defines.append(f"-DVECTOR_DISP={displacement}")
        (directory / "thresholds.bin").write_bytes(bytes.fromhex(vector))
    if validated is not None:
        (directory / "validated.bin").write_bytes(validated)
    library = directory / "display_modes.dylib"
    subprocess.run(
        [
            "/usr/bin/clang",
            "-dynamiclib",
            f"-D{form.upper()}",
            *defines,
            *(["-DVALIDATED_REPLACEMENT"] if validated else []),
            workspace / "tests/compatibility/display_modes.S",
            "-o",
            library,
        ],
        cwd=directory,
        check=True,
    )
    modes = ctypes.CDLL(str(library))
    baseline = modes.validated if validated else None
    for run in [modes.original, modes.patched] + (
        [baseline] if baseline else []
    ):
        run.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.POINTER(ModeResult),
            ctypes.c_void_p,
        ]
        run.restype = None

    # Captured instructions address locals below RBP. The packed comparison
    # uses MOVDQA, which requires 16-byte alignment for its input vector.
    frame_size = -min(
        v for k, v in slots.items() if k not in ("copy_target", "thresholds")
    )
    frame = ctypes.create_string_buffer((frame_size + 15) & ~15)
    rbp = ctypes.addressof(frame) + len(frame)
    mode = ctypes.create_string_buffer(24)
    display = ctypes.create_string_buffer(148)
    mode_pointer = ctypes.addressof(mode)
    registers = ctypes.create_string_buffer(16)
    struct.pack_into("<Q", registers, 0, mode_pointer)
    if "mode" in slots:
        struct.pack_into("<Q", frame, slots["mode"], mode_pointer)
    if "saved_pointer" in slots:
        struct.pack_into("<Q", frame, slots["saved_pointer"], rbp)
        display = (ctypes.c_char * 148).from_address(
            rbp + slots["display_base"]
        )
    saved_edi = 0x87654321
    if "saved_edi" in slots:
        struct.pack_into("<I", frame, slots["saved_edi"], saved_edi)

    widths = (-(2**31), 799, 800, 1919, 1920, 1921, 2**31 - 1)
    heights = (-(2**31), 514, 515, 516, 599, 600, 2**31 - 1)
    format_bytes = generator["LAYOUTS"][form].format_bytes
    format_type = {0: None, 1: "<B", 4: "<I"}[format_bytes]
    formats = (0, 1, 2, 3, 4, 255)
    if format_bytes != 1:
        formats += (256,)
    initial_flags = (0, 3, 0x87654321, 0xFFFFFFFF)
    apple, actual, previous = ModeResult(), ModeResult(), ModeResult()
    for bits in range(64):
        # Valid, safe, television, mirroring, AirPlay, native.
        flags = (bits & 3) | ((bits & 12) << 18) | ((bits & 48) << 20)
        struct.pack_into("<I", mode, 20, flags)
        # Give the display and mode records opposite television and native
        # bits so reading either flag from the wrong record changes the result.
        display_flags = flags ^ (1 << 20) ^ (1 << 25)
        struct.pack_into("<I", display, 144, display_flags)
        input_flags = (
            flags
            if form in ("register_flags", "format_register")
            else display_flags
        )
        saved = saved_edi if "saved_edi" in slots else flags
        if "saved_pointer" in slots:
            saved = rbp
        if "saved_flags" in slots:
            saved = (~flags) & 0xFFFFFFFF
            struct.pack_into("<I", frame, slots["saved_flags"], saved)
        for width, height in itertools.product(widths, heights):
            # For native 1920×515, use Apple's result at 600 lines to check
            # that admitting the shorter mode preserves every other decision.
            native = width == 1920 and height == 515 and flags & (1 << 25)
            struct.pack_into("<i", frame, slots["width"], width)
            for fmt, initial in itertools.product(formats, initial_flags):
                if format_type:
                    struct.pack_into(
                        format_type, frame, slots["format_check"], fmt
                    )
                struct.pack_into("<I", registers, 8, fmt)
                struct.pack_into(
                    "<i",
                    frame,
                    slots.get("height", slots["width"] + 4),
                    600 if native else height,
                )
                modes.original(
                    rbp,
                    display,
                    input_flags,
                    initial,
                    ctypes.byref(apple),
                    registers,
                )
                struct.pack_into(
                    "<i", frame, slots.get("height", slots["width"] + 4), height
                )
                modes.patched(
                    rbp,
                    display,
                    input_flags,
                    initial,
                    ctypes.byref(actual),
                    registers,
                )
                assert actual.classification == apple.classification, (
                    f"{form} {width}x{height}, format {fmt}, flags {flags:#x}, "
                    f"initial {initial:#x}: {actual.classification:#x} != {apple.classification:#x}"
                )
                if form == "packed":
                    tv = display_flags & (1 << 20)
                    expected = (
                        (-1, -1) if tv else (-(width >= 800), -(height >= 600))
                    )
                    assert tuple(actual.lanes) == expected
                if baseline:
                    baseline(
                        rbp,
                        display,
                        input_flags,
                        initial,
                        ctypes.byref(previous),
                        registers,
                    )
                    assert bytes(actual) == bytes(previous)
                assert actual.mode == apple.mode == mode_pointer
                assert actual.saved == apple.saved == saved
                assert (
                    actual.flags
                    == apple.flags
                    == (
                        display_flags if form in ("branch", "packed") else flags
                    )
                )
                if "saved_pointer" in slots:
                    assert actual.other == apple.other == display_flags
                if form == "format_register":
                    assert actual.other == apple.other == fmt


fixtures = json.loads(
    (workspace / "tests/fixtures/core_display.json").read_text()
)

validated_modes = {
    bytes.fromhex(f["original"]): bytes.fromhex(f["replacement"])
    for f in fixtures
}
tested_modes = set()
layout_examples = {}
pages = workspace / "tests/fixtures/core_display_pages"
cases = json.loads((pages / "manifest.json").read_text())
failures = 0
for case in cases:
    form = case["form"]
    page = pages / (case["build"] + ".bin")
    data = page.read_bytes()
    if hashlib.sha256(data).hexdigest() != case["sha256"]:
        raise ValueError(f"Unexpected fixture contents: {page.name}")
    directory = scratch / case["build"]
    directory.mkdir()
    site = case["geometry_offset"]
    result = subprocess.run(
        [checker, "--page", page, str(site), directory],
        capture_output=True,
        text=True,
    )
    print(
        f"{case['version']} ({case['build']}, {case['source']}): {result.stdout.strip() or result.stderr.strip()}",
        flush=True,
    )
    if result.returncode:
        failures += 1
        continue
    original = (directory / "original.bin").read_bytes()
    matched_form, slots = frame_layout(original)
    assert (
        form == matched_form
    ), f"{case['build']}: expected {form}, found {matched_form}"
    layout_examples.setdefault(matched_form, original)
    if matched_form == "register_flags":
        check_relative_format_slot(
            data, site, directory, data.index(original), slots
        )
    pair = original, (directory / "patched.bin").read_bytes()
    if platform.machine() == "x86_64" and pair not in tested_modes:
        check_display_modes(
            matched_form,
            slots,
            directory,
            validated_modes.get(original),
            case.get("threshold_vector"),
        )
        tested_modes.add(pair)
        print(
            f"{case['build']}: mode classifications and live registers match",
            flush=True,
        )
print(f"{len(cases) - failures}/{len(cases)} CoreDisplay pages patched")
if failures:
    sys.exit(1)


def check(original, replacement):
    before, after = scratch / "before.bin", scratch / "after.bin"
    before.write_bytes(original)
    after.write_bytes(replacement)
    subprocess.run([str(checker), str(before), str(after)], check=True)


for fixture in fixtures:
    original = bytes.fromhex(fixture["original"])
    form, slots = frame_layout(original)
    assert form == fixture["form"]
    replacement = assembled_replacement(form, slots, original)
    check(original, replacement)
    print(
        f"{fixture['build']}: runtime output matches the assembled size check"
    )

    if "page" in fixture:
        page = fixture["page"]
        prefix = bytes.fromhex(page["prefix"])
        suffix = bytes.fromhex(page["suffix"])
        assert len(prefix) == page["mode_check_offset"]
        check(prefix + original + suffix, prefix + replacement + suffix)
        print(f"{fixture['build']}: captured page passes control-flow checks")


# A page with two different layouts has no unique patch site.
ambiguous = scratch / "ambiguous.bin"
for left, right in itertools.combinations(layout_examples.values(), 2):
    ambiguous.write_bytes(left + right)
    subprocess.run([checker, ambiguous], check=True)


# Moving and reordering locals exposes any use of a captured stack offset.
# Keep short RBP-relative operands within their signed one-byte range.
for form, original in layout_examples.items():
    _, operands, _, _ = generator["original_pattern"](form)
    names = list(dict.fromkeys(field for field, _, _ in operands))
    depth = 64 if any(size == 1 for _, _, size in operands) else 8192
    for base, order in ((0, names), (depth, reversed(names))):
        frame = {name: -base - 8 * i for i, name in enumerate(order, 1)}
        if "display_base" in frame:
            frame["copy_target"] = -16384
            frame["display_base"] = -240 if base else -256
            frame["format_check"] = frame["display_base"] - 32
        if form == "packed":
            frame["width"] = -48 if not base else -8240
        relocated = bytearray(original)
        for field, offset, width in operands:
            relocated[offset : offset + width] = frame[field].to_bytes(
                width, "little", signed=True
            )
        expected = assembled_replacement(form, frame, relocated)
        check(relocated, expected)
    print(f"{form}: relocated stack slots match independently assembled code")

    for field in (n for n in names if n not in ("copy_target", "thresholds")):
        invalid = bytearray(relocated)
        for local, offset, width in operands:
            if local == field:
                invalid[offset : offset + width] = bytes(width)
        before = scratch / "invalid.bin"
        before.write_bytes(invalid)
        subprocess.run([str(checker), str(before)], check=True)

    overlapping = bytearray(relocated)
    collide = "format_check" if "format_check" in frame else "mode"
    for field, offset, width in operands:
        if field == collide:
            overlapping[offset : offset + width] = frame["width"].to_bytes(
                width, "little", signed=True
            )
    before = scratch / "overlapping.bin"
    before.write_bytes(overlapping)
    subprocess.run([str(checker), str(before)], check=True)

print(
    "All supported layouts preserve page boundaries and reject altered or ambiguous size checks."
)
