#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Fetch checksum-pinned Intel Dock executables from Apple installers."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import lzma
from pathlib import Path
import plistlib
import runpy
import struct
import subprocess
from tempfile import TemporaryDirectory
from time import monotonic
from urllib.request import Request, urlopen
import xml.etree.ElementTree as xml
import zlib


DOCK = "System/Library/CoreServices/Dock.app/Contents/MacOS/Dock"


class RemoteFile:
    def __init__(self, url):
        self.url = url
        self.size = None
        self.downloaded = 0

    def read_at(self, start, size):
        if start < 0 or size <= 0 or self.downloaded + size > 1024**3:
            raise ValueError("Installer range exceeds the 1 GiB download limit")
        span = f"{start}-{start + size - 1}"
        request = Request(
            self.url,
            headers={"Range": f"bytes={span}", "Accept-Encoding": "identity"},
        )
        with urlopen(request, timeout=30) as response:
            interval, _, total = response.headers.get(
                "Content-Range", ""
            ).partition("/")
            if response.status != 206 or interval != f"bytes {span}":
                raise ValueError(
                    "Installer server did not honor the byte range"
                )
            total = int(total)
            if self.size is not None and self.size != total:
                raise ValueError("Installer size changed during extraction")
            self.size = total
            data = response.read(size + 1)
        if len(data) != size:
            raise ValueError("Installer range was truncated")
        self.downloaded += size
        return data


class InstallerImage:
    def __init__(self, asset, offset, size, directory):
        self.asset, self.offset, self.size = asset, offset, size
        self.cache = directory / "chunks"
        self.cache.mkdir()
        self.error = None

    def read(self, start, stop):
        block_size = 1024 * 1024
        if not 0 <= start < stop <= self.size:
            raise ValueError("Disk image read is outside the installer")
        while start < stop:
            index = start // block_size
            offset = index * block_size
            chunk = self.cache / str(index)
            if chunk.is_file():
                data = chunk.read_bytes()
            else:
                data = self.asset.read_at(
                    self.offset + offset, min(block_size, self.size - offset)
                )
                chunk.write_bytes(data)
            end = min(stop, offset + len(data))
            yield data[start - offset : end - offset]
            start = end


def xar_entry(read_at, name):
    header = read_at(0, 28)
    if header[:4] != b"xar!":
        raise ValueError("Installer package is not a XAR archive")
    size, compressed, expanded = struct.unpack_from(">H2xQQ", header, 4)
    if (
        size < 28
        or not 0 < compressed <= 8 * 1024**2
        or expanded > 16 * 1024**2
    ):
        raise ValueError("Invalid installer table of contents")
    toc = zlib.decompress(read_at(size, compressed))
    if len(toc) != expanded:
        raise ValueError("Installer table of contents has the wrong size")
    entries = [
        node
        for node in xml.fromstring(toc).iter("file")
        if node.findtext("name") == name
    ]
    if len(entries) != 1:
        raise ValueError(f"Expected one {name} in the installer package")
    node = entries[0]
    encoding = node.find("data/encoding")
    if encoding is None or encoding.get("style") != "application/octet-stream":
        raise ValueError(f"Unsupported compression for {name}")
    offset, length = int(node.findtext("data/offset")), int(
        node.findtext("data/length")
    )
    if offset < 0 or length <= 0:
        raise ValueError(f"Invalid installer bounds for {name}")
    return size + compressed + offset, length


def extract_payload(core, offset, length, output, checksum):
    core.seek(offset)
    if not core.read(12).startswith(b"pbzx"):
        raise ValueError("Core.pkg does not contain a PBZX payload")
    with (
        output.open("wb") as destination,
        subprocess.Popen(
            ["/usr/bin/tar", "-xqOf", "-", "./" + DOCK],
            stdin=subprocess.PIPE,
            stdout=destination,
            stderr=subprocess.PIPE,
        ) as tar,
    ):
        complete = False
        try:
            while core.tell() < offset + length:
                header = core.read(16)
                if len(header) != 16:
                    raise ValueError("Truncated PBZX chunk header")
                expanded, stored = struct.unpack(">QQ", header)
                if (
                    not 0 < stored <= expanded <= 16 * 1024**2
                    or stored > offset + length - core.tell()
                ):
                    raise ValueError("Invalid PBZX chunk size")
                chunk = core.read(stored)
                if len(chunk) != stored:
                    raise ValueError("Truncated PBZX chunk")
                if stored != expanded:
                    chunk = lzma.decompress(chunk, format=lzma.FORMAT_XZ)
                if len(chunk) != expanded:
                    raise ValueError("PBZX chunk expanded to the wrong size")
                tar.stdin.write(chunk)
                # Some tar versions keep scanning after emitting Dock. Its
                # pinned checksum lets us stop before reading the rest.
                if (
                    output.stat().st_size
                    and hashlib.sha256(output.read_bytes()).hexdigest()
                    == checksum
                ):
                    complete = True
                    tar.terminate()
                    break
        except BrokenPipeError:
            # tar may exit after emitting Dock. The caller still verifies
            # the extracted executable against its pinned checksum.
            pass
        finally:
            try:
                tar.stdin.close()
            except BrokenPipeError:
                pass
        detail = tar.stderr.read().decode(errors="replace")
        if tar.wait() != 0 and not complete:
            raise ValueError("Dock extraction from Core.pkg failed: " + detail)


def extract_catalina(release, directory, mount_recovery):
    asset = RemoteFile(release["url"])
    offset, length = xar_entry(asset.read_at, "InstallESD.dmg")
    image = InstallerImage(asset, offset, length, directory)
    try:
        with mount_recovery(image, directory) as mount:
            with (mount / "Packages/Core.pkg").open("rb") as core:

                def read_at(start, size):
                    core.seek(start)
                    return core.read(size)

                offset, length = xar_entry(read_at, "Payload")
                output = directory / "Dock"
                extract_payload(core, offset, length, output, release["sha256"])
    except (OSError, subprocess.SubprocessError) as error:
        if image.error:
            raise RuntimeError(
                f"Installer image read failed after {asset.downloaded:,} bytes and {len(list(image.cache.iterdir()))} unique blocks: {image.error}"
            ) from error
        raise
    return output, asset.downloaded


def extract_restore(release, directory, ipsw):
    subprocess.run(
        [
            str(ipsw),
            "extract",
            "--remote",
            "--files",
            "--ranged",
            "--pattern",
            r"(^|/)System/Library/CoreServices/(Dock\.app/Contents/MacOS/Dock|SystemVersion\.plist)$",
            "--output",
            str(directory),
            release["url"],
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    versions = list(directory.rglob("SystemVersion.plist"))
    binaries = [
        path
        for path in directory.rglob("Dock")
        if str(path).endswith("/" + DOCK)
    ]
    if len(versions) != 1 or len(binaries) != 1:
        raise ValueError(
            "Restore image did not yield one Dock and SystemVersion.plist"
        )
    version = plistlib.loads(versions[0].read_bytes())
    if (version.get("ProductBuildVersion"), version.get("ProductVersion")) != (
        release["build"],
        release["version"],
    ):
        raise ValueError(
            "Restore image version differs from the pinned fixture"
        )
    return binaries[0]


def verify_intel(data, release, intel_slice):
    image = intel_slice(data)
    thin = data[image.offset : image.offset + image.size]
    if hashlib.sha256(thin).hexdigest() != release["sha256"]:
        raise ValueError(f"{release['build']} Dock Intel SHA-256 mismatch")
    return thin


def fetch(release, args, intel_slice, mount_recovery):
    start = monotonic()
    output = args.output / release["build"] / "Dock"
    if output.is_file():
        verify_intel(output.read_bytes(), release, intel_slice)
        return f"{release['build']} Dock checksum verified (cached)"
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="extract-", dir=output.parent) as temporary:
        directory = Path(temporary)
        if release["build"] == "19H2":
            binary, transferred = extract_catalina(
                release, directory, mount_recovery
            )
            detail = f", {transferred:,} installer bytes read"
        else:
            binary = extract_restore(release, directory, args.ipsw)
            detail = ""
        data = verify_intel(binary.read_bytes(), release, intel_slice)
        staged = directory / "Dock.verified"
        staged.write_bytes(data)
        staged.replace(output)
    return f"{release['build']} Dock checksum verified in {monotonic() - start:.1f}s{detail}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("ipsw", "parser", "recovery", "manifest", "output"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    intel_slice = runpy.run_path(str(args.parser))["intel_slice"]
    mount_recovery = runpy.run_path(str(args.recovery))["mount_recovery"]
    releases = json.loads(args.manifest.read_text())
    failed = False
    with ThreadPoolExecutor(max_workers=4) as workers:
        jobs = [
            workers.submit(fetch, release, args, intel_slice, mount_recovery)
            for release in releases
        ]
        for release, job in zip(releases, jobs):
            try:
                print(job.result(), flush=True)
            except (
                OSError,
                RuntimeError,
                ValueError,
                subprocess.SubprocessError,
            ) as error:
                detail = getattr(error, "stderr", None)
                if isinstance(detail, bytes):
                    detail = detail.decode(errors="replace")
                print(f"{release['build']} failed: {error}", flush=True)
                if detail:
                    print(detail.strip(), flush=True)
                failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
