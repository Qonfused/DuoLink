#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check DuoLink's ASSR hook against Intel framebuffer drivers from Recovery."""

import os
from pathlib import Path
import runpy
import subprocess
import sys


def check_driver(path, symbols, check_hook, intel_slice):
    output = subprocess.check_output(
        ["/usr/bin/nm", "-arch", "x86_64", "-nU", str(path)], text=True
    )
    addresses = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in symbols:
            addresses[fields[2]] = int(fields[0], 16)

    missing = [symbol for symbol in symbols if symbol not in addresses]
    if missing:
        raise ValueError("Missing symbols:\n" + "\n".join(missing))

    # The first symbol printed by check_hook is the method Lilu routes.
    address = addresses[symbols[0]]
    data = path.read_bytes()
    image = intel_slice(data)
    for section in image.sections:
        displacement = address - section.address
        if section.name == "__text" and 0 <= displacement < section.size:
            start = image.offset + section.offset + displacement
            end = start + min(32, section.size - displacement)
            result = subprocess.run([str(check_hook)], input=data[start:end])
            if result.returncode:
                raise ValueError(
                    "WriteAUX entry fails DuoLink's prologue check"
                )
            return

    raise ValueError("WriteAUX is outside the framebuffer's code sections")


def main():
    if len(sys.argv) == 1:
        raise SystemExit("No recovery framebuffer drivers supplied")

    workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
    scratch = Path(os.environ["TEST_TMPDIR"])
    intel_slice = runpy.run_path(str(workspace / "tools/macho.py"))[
        "intel_slice"
    ]

    check_hook = scratch / "check_hook"
    subprocess.run(
        [
            "/usr/bin/clang++",
            "-std=c++14",
            "-I",
            str(workspace),
            str(workspace / "tests/compatibility/check_hook.cpp"),
            "-o",
            str(check_hook),
        ],
        check=True,
    )
    symbols = subprocess.check_output(
        [str(check_hook), "--symbols"], text=True
    ).splitlines()

    failed = False
    for argument in sys.argv[1:]:
        driver = Path(argument)
        build = driver.parent.name
        try:
            check_driver(driver, symbols, check_hook, intel_slice)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            failed = True
            print(f"{build} failed: {error}", file=sys.stderr)
        else:
            print(f"{build} passed")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
