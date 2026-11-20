#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Stamp extension metadata from the version passed to the compiler."""
from pathlib import Path
import plistlib
import sys


def expand(value, settings):
    if isinstance(value, dict):
        return {key: expand(item, settings) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, settings) for item in value]
    if isinstance(value, str):
        for name, replacement in settings.items():
            value = value.replace("$(" + name + ")", replacement)
        if "$(" in value:
            raise ValueError("Unresolved Info.plist build setting: " + value)
    return value


def process(source, destination, executable, version):
    info = plistlib.loads(Path(source).read_bytes())
    identifier = info["CFBundleIdentifier"]
    settings = {
        "EXECUTABLE_NAME": executable,
        "PRODUCT_NAME": executable,
        "PRODUCT_BUNDLE_IDENTIFIER": identifier,
        "MODULE_VERSION": version,
    }
    processed = expand(info, settings)
    for key, expected in [
        ("CFBundleExecutable", executable),
        ("CFBundleIdentifier", identifier),
        ("CFBundleVersion", version),
        ("CFBundleShortVersionString", version),
    ]:
        if processed[key] != expected:
            raise ValueError(key + " does not match the build configuration")
    Path(destination).write_bytes(plistlib.dumps(processed, sort_keys=False))


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: process_info.py SOURCE DESTINATION EXECUTABLE VERSION"
        )
    process(*sys.argv[1:])


if __name__ == "__main__":
    main()
