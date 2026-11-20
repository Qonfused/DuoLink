#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Create a versioned ZIP containing DuoLink and its bundled companion."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import posixpath
import re
import stat
import struct
import zipfile


def create_archive(bundle, documents, version, build_type, output):
    bundles = [bundle, bundle / "Contents/PlugIns/DuoLinkTouch.kext"]
    versions = {}
    for kext in bundles:
        contents = kext / "Contents"
        info = plistlib.loads((contents / "Info.plist").read_bytes())
        executable = contents / "MacOS" / info["CFBundleExecutable"]
        if struct.unpack_from("<II", executable.read_bytes()) != (
            0xFEEDFACF,
            0x01000007,
        ):
            raise ValueError(f"Expected an Intel executable: {kext.name}")
        versions[kext.stem] = info["CFBundleVersion"]
    if versions["DuoLink"] != version:
        raise ValueError("Archive version differs from the DuoLink bundle")

    files = {
        "DuoLink.kext/" + p.relative_to(bundle).as_posix(): p
        for p in bundle.rglob("*")
        if p.is_file()
    }
    files.update(documents)
    payload = {name: path.read_bytes() for name, path in files.items()}
    for name, path in documents.items():
        # Keep bundled guides local and link source files to the release tag.
        def source_link(match):
            destination = match[2]
            if ":" in destination or destination.startswith("#"):
                return match[0]
            target, separator, fragment = destination.partition("#")
            target = posixpath.normpath(
                posixpath.join(posixpath.dirname(name), target)
            )
            if target in payload:
                return match[0]
            url = (
                f"https://github.com/Qonfused/DuoLink/blob/v{version}/{target}"
            )
            return match[1] + url + separator + fragment + match[3]

        payload[name] = re.sub(
            r"(\[[^\]]*\]\()([^)]+)(\))", source_link, path.read_text()
        ).encode()
    metadata = {
        "version": version,
        "build_type": build_type,
        "architecture": "x86_64",
        "bundles": versions,
        "files": {
            name: hashlib.sha256(data).hexdigest()
            for name, data in sorted(payload.items())
        },
    }
    payload["build.json"] = (json.dumps(metadata, indent=2) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            mode = 0o755 if "/Contents/MacOS/" in name else 0o644
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bundle", "output", "checksum"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument(
        "--build-type", choices=("RELEASE", "DEBUG"), required=True
    )
    parser.add_argument("--document", action="append", default=[])
    args = parser.parse_args()
    documents = dict(
        (name, Path(path))
        for name, path in (entry.split("=", 1) for entry in args.document)
    )
    create_archive(
        args.bundle,
        documents,
        args.version,
        args.build_type,
        args.output,
    )
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.checksum.write_text(f"{digest}  {args.output.name}\n")
    print(args.output)


if __name__ == "__main__":
    main()
