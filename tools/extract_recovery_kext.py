#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Extract the Intel framebuffer executable from Apple Internet Recovery."""

import argparse
from bisect import bisect_right
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
from itertools import accumulate
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Thread
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


KEXT = "AppleIntelCFLGraphicsFramebuffer"
BUNDLE_ID = f"com.apple.driver.{KEXT}"
CACHE = Path(__file__).resolve().parents[1] / ".cache" / "recovery-kexts"
BUILD = re.compile(r"[0-9]+[A-Z][0-9]+[a-z]?")
RANGE = re.compile(r"bytes=([0-9]*)-([0-9]*)")
VERSION_PLIST = "System/Library/CoreServices/SystemVersion.plist"


def open_asset(url, token, headers=None):
    address = urlsplit(url)
    host = address.hostname or ""
    if address.scheme not in ("http", "https") or not host.endswith(
        (".apple.com", ".cdn-apple.com")
    ):
        raise ValueError(f"Unexpected recovery asset URL: {url}")

    request = Request(url, headers=headers or {})
    request.add_header("User-Agent", "InternetRecovery/1.0")
    request.add_unredirected_header("Cookie", f"AssetToken={token}")
    return urlopen(request, timeout=30)


def recovery_image(args, directory):
    # OpenCore implements the recovery handshake and Apple's chunklist signature
    # check. Its verifier yields the chunks before it checks the signature.
    if not __debug__:
        raise RuntimeError(
            "Python optimization disables chunklist verification"
        )

    recovery = runpy.run_path(str(args.macrecovery))
    session = recovery["get_session"](argparse.Namespace(verbose=False))
    info = recovery["get_image_info"](
        session, args.board_id, args.mlb, os_type=args.os_type
    )
    chunklist = directory / "BaseSystem.chunklist"
    with open_asset(info["CU"], info["CT"]) as response:
        data = response.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise ValueError("Recovery chunklist exceeds 1 MiB")
    chunklist.write_bytes(data)
    try:
        chunks = tuple(recovery["verify_chunklist"](chunklist))
    except (AssertionError, struct.error) as error:
        raise ValueError("Invalid Apple recovery chunklist") from error
    return RecoveryImage(info["AU"], info["AT"], chunks, directory)


class RecoveryImage:
    def __init__(self, url, token, chunks, directory):
        if not chunks or any(length <= 0 for length, _ in chunks):
            raise ValueError("Recovery chunklist contains an empty chunk table")
        lengths, self.hashes = zip(*chunks)
        self.offsets = tuple(accumulate(lengths, initial=0))
        self.size = self.offsets[-1]
        self.url = url
        self.token = token
        self.directory = directory
        self.downloaded = 0
        self.error = None

    def read(self, start, stop):
        index = bisect_right(self.offsets, start) - 1
        while start < stop:
            offset, end = self.offsets[index : index + 2]
            data = self.chunk(index, offset, end)
            yield data[start - offset : min(stop, end) - offset]
            start = end
            index += 1

    def chunk(self, index, start, stop):
        path = self.directory / f"chunk-{index}"
        if path.is_file():
            return path.read_bytes()

        # Read the entire signed chunk before serving any bytes from it.
        span = f"{start}-{stop - 1}"
        with open_asset(
            self.url,
            self.token,
            {"Range": f"bytes={span}", "Accept-Encoding": "identity"},
        ) as response:
            if (
                response.status != 206
                or response.headers.get("Content-Range")
                != f"bytes {span}/{self.size}"
            ):
                raise ValueError(f"Invalid range response for bytes {span}")
            data = response.read(stop - start + 1)

        if (
            len(data) != stop - start
            or hashlib.sha256(data).digest() != self.hashes[index]
        ):
            raise ValueError(
                f"Recovery chunk {index} failed SHA-256 verification"
            )
        path.write_bytes(data)
        self.downloaded += len(data)
        return data


def request_range(header, size):
    match = RANGE.fullmatch(header)
    if match is None:
        return None

    first, last = match.groups()
    if first:
        start = int(first)
        stop = min(int(last) + 1, size) if last else size
    elif last:
        start, stop = max(0, size - int(last)), size
    else:
        return None

    return (start, stop) if start < stop else None


@contextmanager
def mount_recovery(image, directory):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_HEAD(self):
            self.send_response(200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(image.size))
            self.end_headers()

        def do_GET(self):
            if image.error:
                self.send_error(502)
                return
            span = request_range(self.headers.get("Range", ""), image.size)
            if span is None:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{image.size}")
                self.end_headers()
                return

            start, stop = span
            self.send_response(206)
            self.send_header(
                "Content-Range", f"bytes {start}-{stop - 1}/{image.size}"
            )
            self.send_header("Content-Length", str(stop - start))
            self.end_headers()

            try:
                for data in image.read(start, stop):
                    self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                # DiskImages can abandon a read while detaching the image.
                return
            except (OSError, ValueError) as error:
                image.error = error

    mount = directory / "Recovery"
    with HTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # Verify the signed chunks as DiskImages reads them.
            subprocess.run(
                [
                    "/usr/bin/hdiutil",
                    "attach",
                    "-readonly",
                    "-nobrowse",
                    "-noverify",
                    "-noautofsck",
                    "-mountpoint",
                    str(mount),
                    f"http://127.0.0.1:{server.server_port}/BaseSystem.dmg",
                ],
                stdout=subprocess.DEVNULL,
                check=True,
            )
            yield mount
        finally:
            try:
                if mount.is_mount():
                    # Background readers can keep this private read-only
                    # mount busy after extraction has finished.
                    subprocess.run(
                        ["/usr/bin/hdiutil", "detach", "-force", str(mount)],
                        stdout=subprocess.DEVNULL,
                        check=True,
                    )
            finally:
                server.shutdown()
                thread.join()


def extract_framebuffer(mount, directory):
    extensions = mount / "System" / "Library" / "Extensions"
    binary = extensions / f"{KEXT}.kext" / "Contents" / "MacOS" / KEXT
    if binary.is_file():
        return binary

    collections = sorted((extensions.parent / "KernelCollections").glob("*.kc"))
    if not collections:
        raise RuntimeError("Recovery has no framebuffer or kernel collection")

    ipsw = shutil.which("ipsw")
    if ipsw is None:
        raise RuntimeError(
            "Install ipsw to extract the framebuffer from a kernel collection"
        )

    errors = []
    for collection in collections:
        result = subprocess.run(
            [
                ipsw,
                "kernel",
                "extract",
                str(collection),
                BUNDLE_ID,
                "--arch",
                "x86_64",
                "--output",
                str(directory),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return directory / BUNDLE_ID
        detail = result.stderr.strip() or result.stdout.strip()
        errors.append(f"{collection.name}\n{detail}")
    raise RuntimeError(
        f"{BUNDLE_ID} could not be extracted\n" + "\n".join(errors)
    )


def cache_framebuffer(args):
    if args.build:
        cached = args.cache_dir / args.build / KEXT
        if cached.is_file():
            return cached

    if args.macrecovery is None or args.board_id is None:
        raise ValueError(
            "Provide --macrecovery and --board-id for a cache miss"
        )

    with TemporaryDirectory(prefix="duolink-recovery-") as scratch:
        directory = Path(scratch)
        image = recovery_image(args, directory)
        try:
            with mount_recovery(image, directory) as mount:
                version = plistlib.loads((mount / VERSION_PLIST).read_bytes())
                build = version["ProductBuildVersion"]
                if not BUILD.fullmatch(build):
                    raise ValueError(f"Invalid recovery build: {build!r}")
                if args.build and args.build != build:
                    raise ValueError(
                        f"Apple returned recovery build {build}, "
                        f"expected {args.build}"
                    )

                cached = args.cache_dir / build / KEXT
                if not cached.is_file():
                    binary = extract_framebuffer(mount, directory)
                    subprocess.run(
                        [
                            "/usr/bin/lipo",
                            str(binary),
                            "-verify_arch",
                            "x86_64",
                        ],
                        check=True,
                    )
                    cached.parent.mkdir(parents=True, exist_ok=True)
                    with TemporaryDirectory(dir=cached.parent) as staging:
                        output = Path(staging) / KEXT
                        shutil.copyfile(binary, output)
                        output.replace(cached)
        except (OSError, subprocess.SubprocessError) as error:
            if image.error:
                raise RuntimeError(
                    f"Recovery image read failed: {image.error}"
                ) from error
            raise

        print(
            f"{version['ProductVersion']} ({build}): read "
            f"{image.downloaded:,} of {image.size:,} image bytes",
            file=sys.stderr,
        )
        return cached


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--macrecovery",
        type=Path,
        help="path to OpenCore's macrecovery.py",
    )
    parser.add_argument("--board-id", help="Intel Mac board ID to request")
    parser.add_argument(
        "--mlb", default="0" * 17, help="optional recovery model selector"
    )
    parser.add_argument(
        "--os-type", choices=("default", "latest"), default="default"
    )
    parser.add_argument(
        "--build", help="expected build number, also used for cache lookup"
    )
    parser.add_argument("--cache-dir", type=Path, default=CACHE)
    args = parser.parse_args()
    if args.build and not BUILD.fullmatch(args.build):
        parser.error("--build must be a macOS build number")

    try:
        print(cache_framebuffer(args))
    except (
        OSError,
        RuntimeError,
        ValueError,
        subprocess.SubprocessError,
    ) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
