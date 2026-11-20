#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check the link contract between the two packaged kexts."""

import os
from pathlib import Path
import plistlib
import subprocess


def check_bundle(bundle):
    contents = bundle / "Contents"
    plist = (contents / "Info.plist").read_bytes()
    assert b"$(" not in plist, f"Unexpanded plist value in {bundle.name}"

    info = plistlib.loads(plist)
    executable = contents / "MacOS" / info["CFBundleExecutable"]
    assert (
        info["CFBundleVersion"].encode() + b"\0" in executable.read_bytes()
    ), f"Executable version differs from Info.plist in {bundle.name}"
    imports = subprocess.check_output(
        ["/usr/bin/nm", "-u", executable], text=True
    )
    assert "Voodoo" not in imports, imports

    libraries = info["OSBundleLibraries"]
    assert not any("Voodoo" in name for name in libraries), libraries
    return info


runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
bundle = runfiles / "DuoLink.kext"
main = check_bundle(bundle)
touch = check_bundle(bundle / "Contents/PlugIns/DuoLinkTouch.kext")
assert (
    touch["OSBundleLibraries"][main["CFBundleIdentifier"]]
    == main["OSBundleCompatibleVersion"]
), "DuoLinkTouch must link against the bundled DuoLink"

# Decoder implementation is private to DuoLink; Lilu's reduced Capstone API
# must not be used to inspect CoreDisplay's SIMD instructions.
executable = bundle / "Contents/MacOS/DuoLink"
imports = subprocess.check_output(["/usr/bin/nm", "-u", executable], text=True)
assert not any(line.strip().startswith("_cs_") for line in imports.splitlines())
symbols = subprocess.check_output(["/usr/bin/nm", "-m", executable], text=True)
for name in ("_duolinkDecodeInstruction", "_duolinkDecodeX86"):
    definition = next(
        line for line in symbols.splitlines() if line.endswith(name)
    )
    assert "non-external" in definition or "private external" in definition
licenses = bundle / "Contents/Resources/Licenses"
assert (licenses / "Capstone.txt").is_file()
assert (licenses / "LLVM.txt").is_file()
