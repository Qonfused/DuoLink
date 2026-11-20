#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Assemble CoreDisplay instruction templates with relocatable stack operands."""

import argparse
from pathlib import Path
import re
import tempfile

from macho import MachO, cmd, require


PATTERNS = {
    "branch": """
        8b 8b 90 00 00 00 0f ba e1 14 72 1b
        81 7d {width} 20 03 00 00 7c 1e
        81 7d {height} 58 02 00 00 7c 15
        83 bd {format_check} 04 72 0c
        48 8b b5 {mode} 8b 46 14 eb 25
        41 81 e7 fc ff ff bf
        48 8b b5 {mode} 8b 46 14 89 c2 83 e2 02 c1 e2 1c
        44 09 fa 81 ca 00 00 00 40 41 89 d7
        a8 01 75 2b 89 c2 f7 d2 41 81 e7 fc ff ff 1f
        f7 c2 02 00 20 00 74 08 f7 c2 02 00 00 01 75 09
        41 81 cf 00 00 00 40 eb 07 41 81 cf 00 00 00 80
    """,
    "setcc": """
        41 0f ba e6 14 72 27
        81 bd {width} 20 03 00 00 0f 9d c0
        81 bd {height} 58 02 00 00 0f 9d c1 20 c1
        22 8d {format_check} 80 f9 01 75 0c
        48 8b 95 {mode} 8b 42 14 eb 22
        81 e3 fc ff ff bf
        48 8b 95 {mode} 8b 42 14 89 c1 83 e1 02 c1 e1 1c 09 d9
        81 c9 00 00 00 40 89 cb a8 01
        8b bd {saved_edi} 75 28 89 c1 f7 d1 81 e3 fc ff ff 1f
        f7 c1 02 00 20 00 74 08 f7 c1 02 00 00 01 75 08
        81 cb 00 00 00 40 eb 06 81 cb 00 00 00 80
    """,
}

# Unique markers locate each stack displacement in assembled code. A second
# assembly with one changed marker verifies every recorded operand position.
MARKERS = {
    "branch": {
        "width": -120,
        "height": -116,
        "format_check": -304,
        "mode": -552,
    },
    "setcc": {
        "width": -4352,
        "height": -4608,
        "format_check": -4864,
        "mode": -5120,
        "saved_edi": -5376,
    },
}


def displacement_size(form, field):
    return 1 if form == "branch" and field in ("width", "height") else 4


def original_pattern(form):
    data, operands = bytearray(), []
    for token in PATTERNS[form].split():
        if token.startswith("{"):
            field = token[1:-1]
            width = displacement_size(form, field)
            operands.append((field, len(data), width))
            data.extend(bytes(width))
        else:
            data.append(int(token, 16))
    return bytes(data), operands


def assemble(template, fields, size, clang, directory):
    source = template.read_text()
    for name, value in {**fields, "size": size}.items():
        source = source.replace(f"@{name}@", str(value))
    require(
        not re.search(r"@[a-z_]+@", source), "unresolved assembly placeholder"
    )
    assembly, obj = directory / "mode.s", directory / "mode.o"
    assembly.write_text(source)
    cmd(clang, "-arch", "x86_64", "-c", str(assembly), "-o", str(obj))
    data = obj.read_bytes()
    section = MachO(data, 0, len(data)).section("__text")
    require(section.size == size, "CoreDisplay replacement length changed")
    return data[section.offset : section.offset + section.size]


def replacement_pattern(form, template, size, clang, directory):
    fields = MARKERS[form]
    data = assemble(template, fields, size, clang, directory)
    operands = []
    for field, value in fields.items():
        width = displacement_size(form, field)
        marker = value.to_bytes(width, "little", signed=True)
        offsets = [
            i
            for i in range(len(data) - width + 1)
            if data[i : i + width] == marker
        ]
        require(offsets, f"missing {field} operand")
        alternative = value - 4
        expected = bytearray(data)
        for offset in offsets:
            expected[offset : offset + width] = alternative.to_bytes(
                width, "little", signed=True
            )
            operands.append((field, offset, width))
        actual = assemble(
            template, {**fields, field: alternative}, size, clang, directory
        )
        require(actual == expected, f"ambiguous {field} instruction operand")
    operands.sort(key=lambda slot: slot[1])
    data = bytearray(data)
    end = 3
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
    return (
        f"constexpr Operand {name}[] = {{"
        + ", ".join(
            f"{{Local::{field.title().replace('_', '')}, {offset}, {width}}}"
            for field, offset, width in operands
        )
        + "};"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", required=True)
    for name in ("branch-template", "setcc-template", "header"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    declarations, checks, sizes = [], [], []
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        for form, template in (
            ("branch", args.branch_template),
            ("setcc", args.setcc_template),
        ):
            original, inputs = original_pattern(form)
            replacement, outputs = replacement_pattern(
                form, template, len(original), args.clang, directory
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
                f"{{{form}Original, {form}Replacement, sizeof({form}Original), "
                f"{form}Inputs, {len(inputs)}, {form}Outputs, {len(outputs)}, "
                f"{4 if form == 'branch' else 1}}}"
            )
            sizes.append(len(original))
    args.header.write_text(
        "// Generated from DuoLink's assembly templates; no host OS data.\n#pragma once\n"
        "namespace duolink { namespace display {\n"
        + "\n".join(declarations)
        + "\nconstexpr ModeCheck modeChecks[] = {"
        + ", ".join(checks)
        + "};\n"
        + f"constexpr size_t maxWindowSize = {max(sizes)};\n"
        + "} }\n"
    )


if __name__ == "__main__":
    main()
