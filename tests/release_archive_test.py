#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check installable archive contents, metadata, and deterministic packaging."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import runpy
import tempfile
import zipfile


workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
package = runpy.run_path(str(workspace / "tools/package_release.py"))[
    "create_archive"
]
bundle = workspace / "DuoLink.kext"
info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
version = info["CFBundleVersion"]

with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    output = directory / f"DuoLink-{version}-RELEASE.zip"
    documents = {
        "LICENSE": workspace / "LICENSE",
        "README.md": workspace / "README.md",
    }
    guide = directory / "guide.md"
    guide.write_text("[Source](../src/patch.hpp#L1) [License](../LICENSE)\n")
    documents["docs/guide.md"] = guide
    package(bundle, documents, version, "RELEASE", output)
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        main = "DuoLink.kext/Contents/MacOS/DuoLink"
        companion = "DuoLink.kext/Contents/PlugIns/DuoLinkTouch.kext/Contents/MacOS/DuoLinkTouch"
        assert main in names and companion in names
        assert not any(name.startswith("DuoLinkTouch.kext/") for name in names)
        assert not any("Voodoo" in name or "SESSION-" in name for name in names)
        for executable in (main, companion):
            assert (
                archive.getinfo(executable).external_attr >> 16 & 0o777 == 0o755
            )
        guide = archive.read("docs/guide.md").decode()
        assert (
            f"https://github.com/Qonfused/DuoLink/blob/v{version}/src/patch.hpp#L1"
            in guide
        )
        assert "[License](../LICENSE)" in guide
        metadata = json.loads(archive.read("build.json"))
        assert metadata["version"] == version
        assert metadata["build_type"] == "RELEASE"
        assert metadata["architecture"] == "x86_64"
        assert "ui_profile" not in metadata
        assert metadata["bundles"]["DuoLink"] == version
        assert set(metadata["files"]) == set(names) - {"build.json"}
        for name, digest in metadata["files"].items():
            assert hashlib.sha256(archive.read(name)).hexdigest() == digest

    repeated = directory / "repeated.zip"
    package(
        bundle,
        dict(reversed(list(documents.items()))),
        version,
        "RELEASE",
        repeated,
    )
    assert output.read_bytes() == repeated.read_bytes()
    try:
        package(bundle, documents, "0.0.0", "RELEASE", output)
    except ValueError as error:
        assert "Archive version differs" in str(error)
    else:
        raise AssertionError("A mismatched release version must be rejected")

print(
    "Nested companion, executable modes, hashes, and repeatable ZIPs verified."
)
