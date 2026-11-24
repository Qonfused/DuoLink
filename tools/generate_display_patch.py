#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Assemble CoreDisplay instruction templates with relocatable stack operands."""

import argparse
from pathlib import Path
import tempfile
from typing import NamedTuple

from macho import MachO, cmd, require


class Layout(NamedTuple):
    instructions: str
    format_bytes: int  # Size of the stack read, or zero for a register operand.


# Loading mode flags through R12 takes one more byte than through R13.
# Each branch that crosses the load must account for its encoded length.
def register_mode_pattern(load, store):
    load_size = len(bytes.fromhex(load))
    return Layout(
        f"""
        48 8d 9d {{display_base:4}} 89 73 e8 {store} 44 89 4b f0
        89 43 e0 ba 81 00 00 00 48 89 df 4c 89 c6 e8 {{copy_target:4}}
        BEGIN
        8b 8b 90 00 00 00 0f ba e1 14 72 1b
        81 7d {{width:1}} 20 03 00 00 7c {20 + load_size:02x}
        81 7d {{height:1}} 58 02 00 00 7c {11 + load_size:02x}
        83 bd {{format_check:4}} 04 72 {2 + load_size:02x}
        {load} eb {27 + load_size:02x}
        41 81 e7 fc ff ff bf {load} 89 c2 83 e2 02 c1 e2 1c
        44 09 fa 81 ca 00 00 00 40 41 89 d7
        VALIDITY 48 8b bd {{saved_pointer:4}} a8 01 JNZ_FORWARD
        """,
        format_bytes=4,
    )


# The matcher reads the entire pattern. DuoLink writes from BEGIN (or the
# first byte) up to VALIDITY, where CoreDisplay tests the driver's flags.
# A placeholder such as {width:1} occupies one displacement byte.
LAYOUTS = {
    "branch_r12": register_mode_pattern("41 8b 44 24 14", "89 7b ec"),
    "branch_r13": register_mode_pattern("41 8b 45 14", "44 89 53 ec"),
    "branch": Layout(
        """
        8b 8b 90 00 00 00 0f ba e1 14 72 1b
        81 7d {width:1} 20 03 00 00 7c 1e
        81 7d {height:1} 58 02 00 00 7c 15
        83 bd {format_check:4} 04 72 0c
        48 8b b5 {mode:4} 8b 46 14 eb 25
        41 81 e7 fc ff ff bf
        48 8b b5 {mode:4} 8b 46 14 89 c2 83 e2 02 c1 e2 1c
        44 09 fa 81 ca 00 00 00 40 41 89 d7
        VALIDITY a8 01 JNZ_FORWARD
        """,
        format_bytes=4,
    ),
    "setcc": Layout(
        """
        41 0f ba e6 14 72 27
        81 bd {width:4} 20 03 00 00 0f 9d c0
        81 bd {height:4} 58 02 00 00 0f 9d c1 20 c1
        22 8d {format_check:4} 80 f9 01 75 0c
        48 8b 95 {mode:4} 8b 42 14 eb 22
        81 e3 fc ff ff bf
        48 8b 95 {mode:4} 8b 42 14 89 c1 83 e1 02 c1 e1 1c 09 d9
        81 c9 00 00 00 40 89 cb
        VALIDITY a8 01 8b bd {saved_edi:4} JNZ_FORWARD
        """,
        format_bytes=1,
    ),
    "register_flags": Layout(
        """
        41 0f ba e7 14 72 3f
        81 bd {width:4} 20 03 00 00 0f 9d c0
        81 bd {height:4} 58 02 00 00 0f 9d c1 20 c1
        84 8d {format_check:4} 75 1b
        41 81 e6 fc ff ff bf 44 89 f8 83 e0 02 c1 e0 1c
        44 09 f0 0d 00 00 00 40 41 89 c6
        48 8b 85 {mode:4} 8b 8d {saved_flags:4}
        VALIDITY 41 f6 c7 01 JNZ_FORWARD
        """,
        format_bytes=1,
    ),
    "format_register": Layout(
        """
        0f ba e3 14 72 41
        81 bd {width:4} 20 03 00 00 48 8b bd {mode:4} 7c 12
        81 bd {height:4} 58 02 00 00 7c 06 41 83 fc 03 77 23
        41 81 e6 fc ff ff bf 89 d8 83 e0 02 c1 e0 1c 44 09 f0
        0d 00 00 00 40 41 89 c6 eb 07 48 8b bd {mode:4}
        VALIDITY 8b 85 {saved_flags:4} f6 c3 01 JNZ_FORWARD
        """,
        format_bytes=0,
    ),
    "packed": Layout(
        """
        41 0f ba e7 14 72 28
        66 0f 6f 85 {width:4} 66 0f 66 05 {thresholds:4}
        66 0f 38 25 c0 66 0f 50 c0 3c 03 0f 94 c0
        22 85 {format_check:4} 3c 01 75 0c
        48 8b 95 {mode:4} 8b 42 14 eb 25
        41 81 e5 fc ff ff bf 48 8b 95 {mode:4} 8b 42 14
        89 c1 83 e1 02 c1 e1 1c 44 09 e9 81 c9 00 00 00 40 41 89 cd
        VALIDITY a8 01 JNZ_FORWARD
        """,
        format_bytes=1,
    ),
}


def original_pattern(form):
    data, operands = bytearray(), []
    start = 0
    for token in LAYOUTS[form].instructions.split():
        if token == "BEGIN":
            start = len(data)
        elif token == "VALIDITY":
            end = len(data)
        elif token == "JNZ_FORWARD":
            data.extend((0x75, 1))  # matchWindow accepts any positive rel8.
        elif token.startswith("{"):
            field, size = token[1:-1].split(":")
            width = int(size)
            require(width in (1, 4), f"invalid displacement size for {field}")
            operands.append((field, len(data), width))
            data.extend(bytes(width))
        else:
            data.append(int(token, 16))
    return bytes(data), operands, start, end


def assembly_source(form, template, clang):
    return cmd(
        clang,
        "-arch",
        "x86_64",
        "-E",
        "-P",
        "-x",
        "assembler-with-cpp",
        f"-D{form.upper()}",
        str(template),
    )


def assemble(source, fields, size, clang, directory):
    for name, value in {**fields, "size": size}.items():
        source = source.replace(f"@{name}@", str(value))
    assembly, obj = directory / "mode.s", directory / "mode.o"
    assembly.write_text(source)
    cmd(
        clang,
        "-arch",
        "x86_64",
        "-c",
        str(assembly),
        "-o",
        str(obj),
    )
    data = obj.read_bytes()
    section = MachO(data, 0, len(data)).section("__text")
    require(section.size == size, "CoreDisplay replacement length changed")
    return data[section.offset : section.offset + section.size]


def replacement_pattern(source, size, captured, clang, directory):
    names = set(captured)
    if "width" in captured:
        names.update(name + "_delta" for name in captured)
    if {"format_check", "display_base"} <= captured:
        names.add("display_flags")
    fields = {name: -64 for name in sorted(names) if f"@{name}@" in source}
    data = assemble(source, fields, size, clang, directory)
    operands = []
    for field in fields:
        actual = assemble(source, {**fields, field: 64}, size, clang, directory)
        # Changing -64 to 64 changes every byte of a signed displacement.
        # Consume four-byte encodings first since both widths start with C0.
        expected = bytearray(data)
        for width in (4, 1):
            marker = (-64).to_bytes(width, "little", signed=True)
            changed = (64).to_bytes(width, "little", signed=True)
            for i in range(size - width + 1):
                if (
                    expected[i : i + width] == marker
                    and actual[i : i + width] == changed
                ):
                    expected[i : i + width] = changed
                    operands.append((field, i, width))
        require(
            actual != data and actual == expected,
            f"could not locate every displacement for {field}",
        )
    operands.sort(key=lambda slot: slot[1])
    data = bytearray(data)
    end = 0
    for field, offset, width in operands:
        require(
            end <= offset < offset + width <= size,
            "overlapping instruction operands",
        )
        data[offset : offset + width] = bytes(width)
        end = offset + width
    return bytes(data), operands


def emit_array(name, data):
    return (
        f"constexpr uint8_t {name}[] = {{"
        + ", ".join(f"0x{x:02x}" for x in data)
        + "};"
    )


def emit_operands(name, operands):
    def initializer(field, offset, width):
        name = field.removesuffix("_delta")
        addressing = ", true" if name != field else ""
        if field == "display_flags":
            # The format code is at display - 32, the flags at display + 144.
            name = "format_check"
            addressing = ", false, 144 + 32"
        member = name.title().replace("_", "")
        return f"{{Field::{member}, {offset}, {width}{addressing}}}"

    return (
        f"constexpr Operand {name}[] = {{"
        + ", ".join(
            initializer(field, offset, width)
            for field, offset, width in operands
        )
        + "};"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()
    declarations, checks, sizes = [], [], []
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        for form, layout in LAYOUTS.items():
            original, inputs, start, end = original_pattern(form)
            captured = {field for field, _, _ in inputs}
            valid_sizes = (1, 4) if "format_check" in captured else (0,)
            require(
                layout.format_bytes in valid_sizes,
                f"{form}: invalid format operand size",
            )
            write_size = end - start
            source = assembly_source(form, args.template, args.clang)
            replacement, outputs = replacement_pattern(
                source, write_size, captured, args.clang, directory
            )
            replacement = original[:start] + replacement + original[end:]
            outputs = (
                [slot for slot in inputs if slot[1] < start]
                + [(f, start + i, n) for f, i, n in outputs]
                + [slot for slot in inputs if slot[1] >= end]
            )
            require(
                len(original) >= 3
                and all(offset >= 3 for _, offset, _ in inputs + outputs),
                f"{form}: the three-byte instruction prefix must be fixed",
            )
            if "thresholds" in captured:
                # PCMPGTD addresses its thresholds relative to the next
                # instruction. Moving its displacement would change the target.
                original_disp = next(
                    i for field, i, _ in inputs if field == "thresholds"
                )
                patched_disp = next(
                    i for field, i, _ in outputs if field == "thresholds"
                )
                require(
                    original_disp == patched_disp,
                    "packed threshold reference moved",
                )
            declarations.extend(
                (
                    emit_array(form + "Original", original),
                    emit_array(form + "Replacement", replacement),
                    emit_operands(form + "Inputs", inputs),
                    emit_operands(form + "Outputs", outputs),
                )
            )
            checks.append(
                f"{{{form}Original, {form}Replacement, sizeof({form}Original), {start}, {write_size}, "
                f"{form}Inputs, {len(inputs)}, {form}Outputs, {len(outputs)}, "
                f"{layout.format_bytes}}}"
            )
            sizes.append(write_size)
    args.header.write_text(
        "// Generated by tools/generate_display_patch.py.\n#pragma once\n"
        "namespace duolink { namespace display {\n"
        + "\n".join(declarations)
        + "\nconstexpr ModeCheck modeChecks[] = {"
        + ", ".join(checks)
        + "};\n"
        + f"constexpr size_t maxWriteSize = {max(sizes)};\n"
        + "} }\n"
    )


if __name__ == "__main__":
    main()
