#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check the one-byte placement patch against original or decoded Intel Dock files."""

import argparse
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys


def decode_fixture(data, image, decoder):
    thin = bytearray(data[image.offset : image.offset + image.size])
    if image.encrypted:
        raise ValueError("Unsupported Mach-O encryption command")
    cursor = 32
    for _ in range(struct.unpack_from("<I", thin, 16)[0]):
        command, length = struct.unpack_from("<II", thin, cursor)
        if command == 0x19:
            flags = struct.unpack_from("<I", thin, cursor + 68)[0]
            if flags & 8:
                offset, size = struct.unpack_from("<QQ", thin, cursor + 40)
                end = offset + size
                start = offset + 3 * 4096
                if size % 4096 or end > len(thin) or start >= end:
                    raise ValueError("Invalid protected segment bounds")
                if thin[start : start + 4] != bytes.fromhex("40 cf 69 2e"):
                    raise ValueError("Unknown protected segment encoding")
                result = subprocess.run(
                    [str(decoder)],
                    input=thin[start:end],
                    capture_output=True,
                    check=True,
                )
                if len(result.stdout) != end - start:
                    raise ValueError(
                        "Protected segment decoder returned the wrong length"
                    )
                thin[start:end] = result.stdout
                struct.pack_into("<I", thin, cursor + 68, flags & ~8)
        cursor += length
    return bytes(thin)


def check_fixture(dock, scratch, verifier, macho, decoder):
    data = dock.read_bytes()
    image = macho["intel_slice"](data)
    data = decode_fixture(data, image, decoder)
    image = macho["intel_slice"](data)
    text = image.section("__text")
    thin = scratch / "Dock.original"
    patched = scratch / "Dock.patched"
    thin.write_bytes(data[image.offset : image.offset + image.size])
    offset = int(
        subprocess.check_output(
            [
                str(verifier),
                str(thin),
                str(text.offset),
                str(text.size),
                str(patched),
            ],
            text=True,
        ).strip()
    )
    original = thin.read_bytes()
    replacement = patched.read_bytes()
    if replacement != original[:offset] + b"\xeb" + original[offset + 1 :]:
        raise ValueError("replacement must change exactly one byte")

    # Disassemble the surrounding block independently of the production matcher.
    address = text.address + offset - text.offset
    cleanup = bytes.fromhex("48 83 c4 20")
    start = offset - (11 if original[offset - 6 : offset - 2] == cleanup else 7)
    end = offset + 2 + original[offset + 1] + 16
    blocks = []
    for content in (original, replacement):
        obj = scratch / "placement.o"
        obj.write_bytes(
            macho["object_code"](
                content[start:end], text.address + start - text.offset
            )
        )
        output = macho["cmd"]("/usr/bin/objdump", "-d", str(obj))
        blocks.append(list(macho["instructions"](output)))
    before = next(i for i in blocks[0] if i.address == address)
    after = next(i for i in blocks[1] if i.address == address)
    if (
        before.mnemonic != "jne"
        or after.mnemonic != "jmp"
        or before.target != after.target
    ):
        raise ValueError("replacement must preserve the fallback branch target")
    if before.target not in {i.address for i in blocks[0]}:
        raise ValueError("fallback is not an instruction boundary")

    # The matched branch must immediately follow this import's result check.
    stubs = macho["cmd"](
        "/usr/bin/objdump", "--macho", "--indirect-symbols", str(thin)
    )
    query = {
        int(fields[0], 16)
        for line in stubs.splitlines()
        if len(fields := line.split()) == 3
        and fields[2] == "_CGGetDisplaysWithRect"
    }
    index = next(
        i for i, insn in enumerate(blocks[0]) if insn.address == address
    )
    preceding = blocks[0][max(0, index - 3) : index]
    if not any(i.mnemonic == "callq" and i.target in query for i in preceding):
        raise ValueError(
            "placement branch does not check CGGetDisplaysWithRect"
        )
    return address, before.target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blowfish", type=Path, required=True)
    parser.add_argument("fixtures", metavar="[BUILD=]DOCK", nargs="+")
    args = parser.parse_args()
    workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
    scratch = Path(os.environ["TEST_TMPDIR"])
    macho = runpy.run_path(str(workspace / "tools/macho.py"))
    decoder = scratch / "decode_dock"
    subprocess.run(
        [
            "/usr/bin/clang",
            "-O2",
            "-I",
            str(args.blowfish.parent),
            str(workspace / "tests/compatibility/decode_dock.c"),
            str(args.blowfish),
            "-o",
            str(decoder),
        ],
        check=True,
    )
    verifier = scratch / "check_dock"
    subprocess.run(
        [
            "/usr/bin/clang++",
            "-std=c++14",
            "-I",
            str(workspace),
            str(workspace / "tests/compatibility/check_dock.cpp"),
            "-o",
            str(verifier),
        ],
        check=True,
    )
    failed = False
    for fixture in args.fixtures:
        build, separator, path = fixture.partition("=")
        if not separator:
            path = fixture
            build = Path(path).parent.name
        if not (build and path):
            parser.error(f"expected [BUILD=]DOCK, got {fixture}")
        try:
            address, target = check_fixture(
                Path(path), scratch, verifier, macho, decoder
            )
        except (
            OSError,
            ValueError,
            StopIteration,
            struct.error,
            subprocess.CalledProcessError,
        ) as error:
            print(f"{build} failed: {error}", file=sys.stderr, flush=True)
            failed = True
        else:
            print(
                f"{build} passed: jne → jmp at 0x{address:x}, fallback 0x{target:x}",
                flush=True,
            )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
