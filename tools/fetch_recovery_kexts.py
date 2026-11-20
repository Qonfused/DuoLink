#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Fetch the pinned Intel framebuffer drivers in parallel."""

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
from time import monotonic


def main():
    extractor, macrecovery, ipsw, cache, releases = sys.argv[1:]
    releases = json.loads(releases)
    env = os.environ.copy()
    env["PATH"] = f"{Path(ipsw).parent}{os.pathsep}{env['PATH']}"
    command = [
        sys.executable,
        extractor,
        "--macrecovery",
        macrecovery,
        "--cache-dir",
        cache,
    ]

    def fetch(release):
        _, build, board, os_type = release
        start = monotonic()
        result = subprocess.run(
            command
            + ["--board-id", board, "--os-type", os_type, "--build", build],
            env=env,
            capture_output=True,
            text=True,
        )
        return result, monotonic() - start

    failed = False
    with ThreadPoolExecutor(max_workers=len(releases)) as workers:
        for release, (result, elapsed) in zip(
            releases, workers.map(fetch, releases)
        ):
            name, build, _, _ = release
            status = "failed" if result.returncode else "fetched"
            print(f"{name} ({build}) {status} in {elapsed:.1f}s")
            detail = result.stderr or (
                result.stdout if result.returncode else ""
            )
            if detail:
                print(detail.strip())
            failed |= result.returncode != 0

    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
