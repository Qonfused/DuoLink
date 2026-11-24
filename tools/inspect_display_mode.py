#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Locate the desktop size check in an Intel CoreDisplay binary for fixture review."""

import argparse
import re
import struct
from pathlib import Path

from macho import cmd, instructions, intel_slice, require, unique


def mode_instructions(binary):
    symbols = []
    for line in cmd(
        "/usr/bin/nm", "-arch", "x86_64", "-an", str(binary)
    ).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in ("t", "T"):
            symbols.append((int(fields[0], 16), fields[2]))
    start = unique(
        {
            addr
            for addr, name in symbols
            if name.startswith("__ZL27description_for_mode_number")
        },
        "mode-description function",
    )
    stop = min(addr for addr, _ in symbols if addr > start)
    return list(
        instructions(
            cmd(
                "/usr/bin/objdump",
                "--arch=x86_64",
                "-d",
                "--x86-asm-syntax=intel",
                "--print-imm-hex",
                f"--start-address={start:#x}",
                f"--stop-address={stop:#x}",
                str(binary),
            )
        )
    )


def geometry(block, image, data):
    comparisons = [ins for ins in block if ins.mnemonic == "cmp"]
    widths = [ins for ins in comparisons if ins.operands.endswith(", 0x320")]
    if len(widths) == 1 and any(
        ins.address > widths[0].address and ins.operands.endswith(", 0x258")
        for ins in comparisons
    ):
        return widths[0], (
            "scalar SETcc"
            if any(ins.mnemonic == "setge" for ins in block)
            else "scalar branches"
        )

    # PCMPGTD uses a strict signed comparison, so thresholds of 799 and 599
    # in the width and height lanes implement the 800×600 minimum.
    for ins in block:
        if ins.mnemonic != "pcmpgtd":
            continue
        displacement = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.operands)
        if not displacement:
            continue
        address = ins.end + int(displacement[1] + displacement[2], 0)
        for section in image.sections:
            if (
                section.address <= address
                and address + 16 <= section.address + section.size
            ):
                offset = (
                    image.offset + section.offset + address - section.address
                )
                if struct.unpack_from("<2i", data, offset) == (799, 599):
                    return ins, "packed signed compare"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    image = intel_slice(data)
    code = mode_instructions(args.binary)
    candidates = []
    for i, ins in enumerate(code[:-1]):
        if ins.mnemonic != "bt" or not ins.operands.endswith(", 0x14"):
            continue
        skip = code[i + 1]
        if (
            skip.mnemonic != "jb"
            or skip.target is None
            or skip.target <= skip.end
        ):
            continue
        flag_update = next(
            (
                j
                for j in range(i + 2, len(code))
                if code[j].mnemonic == "and"
                and code[j].operands.endswith(", 0xbffffffc")
            ),
            None,
        )
        if flag_update is None:
            continue
        block = [
            op for op in code[i + 2 : flag_update] if op.address < skip.target
        ]
        candidate = geometry(block, image, data)
        if candidate:
            candidates.append(candidate)
    site, form = unique(candidates, "desktop size check")
    require(image.uuid is not None, "missing CoreDisplay UUID")
    print(
        f"{image.uuid}: {form} at {site.address:#x}, page offset {site.address % 4096:#x}"
    )


if __name__ == "__main__":
    main()
