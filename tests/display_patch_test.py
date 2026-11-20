#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Compare runtime CoreDisplay patches with validated and reassembled code."""

import json
import os
from pathlib import Path
import runpy
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

fixtures = json.loads(
    (workspace / "tests/fixtures/core_display.json").read_text()
)


def check(original, replacement):
    before, after = scratch / "before.bin", scratch / "after.bin"
    before.write_bytes(original)
    after.write_bytes(replacement)
    subprocess.run([str(checker), str(before), str(after)], check=True)


for fixture in fixtures:
    original = bytes.fromhex(fixture["original"])
    replacement = bytes.fromhex(fixture["replacement"])
    check(original, replacement)
    print(
        f"{fixture['build']}: runtime output matches the validated replacement"
    )

    if "page" in fixture:
        page = fixture["page"]
        prefix = bytes.fromhex(page["prefix"])
        suffix = bytes.fromhex(page["suffix"])
        assert len(prefix) == page["mode_check_offset"]
        check(prefix + original + suffix, prefix + replacement + suffix)
        print(f"{fixture['build']}: captured page passes control-flow checks")

    form = fixture["form"]
    pattern, operands = generator["original_pattern"](form)
    variants = {
        "branch": {
            "width": -100,
            "height": -96,
            "format_check": -236,
            "mode": -832,
        },
        "setcc": {
            "width": -640,
            "height": -636,
            "format_check": -632,
            "mode": -832,
            "saved_edi": -612,
        },
    }
    fields = variants[form]
    extra_frames = {
        "branch": {"width": -4, "height": -8, "format_check": -16, "mode": -32},
        "setcc": {
            "width": -8192,
            "height": -8188,
            "format_check": -8177,
            "mode": -8208,
            "saved_edi": -8164,
        },
    }
    template = workspace / (
        "src/display/display_patch.s.in"
        if form == "branch"
        else "src/display/display_mode.s.in"
    )
    for frame in (extra_frames[form], fields):
        relocated = bytearray(pattern)
        for field, offset, width in operands:
            relocated[offset : offset + width] = frame[field].to_bytes(
                width, "little", signed=True
            )
        expected = generator["assemble"](
            template, frame, len(pattern), "/usr/bin/clang", scratch
        )
        check(relocated, expected)
    print(f"{form}: relocated stack slots match independently assembled code")

    for field in fields:
        invalid = bytearray(relocated)
        for local, offset, width in operands:
            if local == field:
                invalid[offset : offset + width] = bytes(width)
        # An invalid stack offset must not identify an eligible instruction block.
        before = scratch / "invalid.bin"
        before.write_bytes(invalid)
        subprocess.run([str(checker), str(before)], check=True)

    overlapping = bytearray(relocated)
    for field, offset, width in operands:
        if field == "format_check":
            overlapping[offset : offset + width] = fields["width"].to_bytes(
                width, "little", signed=True
            )
    before = scratch / "overlapping.bin"
    before.write_bytes(overlapping)
    subprocess.run([str(checker), str(before)], check=True)

print(
    "Both code forms preserve page boundaries and reject altered or ambiguous blocks."
)
