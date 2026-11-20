#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Reject unbounded installer responses and altered Dock fixtures."""

from contextlib import nullcontext, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
spec = importlib.util.spec_from_file_location(
    "dock_fetcher", workspace / "tools/fetch_dock_binaries.py"
)
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)
parser = runpy.run_path(str(workspace / "tools/macho.py"))


class DockFixtures(unittest.TestCase):
    def test_range_refusal_does_not_read_the_installer(self):
        for status, content_range in ((200, ""), (206, "bytes 0-99/100")):
            with self.subTest(status=status):
                response = SimpleNamespace(
                    status=status,
                    headers={"Content-Range": content_range},
                    read=Mock(
                        side_effect=AssertionError(
                            "must not read a whole installer"
                        )
                    ),
                )
                with patch.object(
                    fetcher, "urlopen", return_value=nullcontext(response)
                ):
                    with self.assertRaises(ValueError):
                        fetcher.RemoteFile(
                            "https://swcdn.apple.com/fixture"
                        ).read_at(0, 1)
                response.read.assert_not_called()

    def test_truncated_range_is_rejected(self):
        response = SimpleNamespace(
            status=206,
            headers={"Content-Range": "bytes 0-3/100"},
            read=Mock(return_value=b"abc"),
        )
        with patch.object(
            fetcher, "urlopen", return_value=nullcontext(response)
        ):
            with self.assertRaisesRegex(ValueError, "truncated"):
                fetcher.RemoteFile("https://swcdn.apple.com/fixture").read_at(
                    0, 4
                )

    def test_checksum_covers_the_original_intel_slice(self):
        thin = parser["object_code"](b"\xc3")
        release = {
            "build": "22TEST",
            "sha256": hashlib.sha256(thin).hexdigest(),
        }
        offset = 4096
        fat = struct.pack(">II", 0xCAFEBABE, 1) + struct.pack(
            ">IIIII", 0x01000007, 3, offset, len(thin), 12
        )
        fat = fat.ljust(offset, b"\0") + thin
        for data in (thin, fat):
            with self.subTest(universal=data is fat):
                self.assertEqual(
                    fetcher.verify_intel(data, release, parser["intel_slice"]),
                    thin,
                )
                altered = data[:-1] + b"\x90"
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    fetcher.verify_intel(
                        altered, release, parser["intel_slice"]
                    )

    def test_failed_extraction_reports_why_the_process_stopped(self):
        errors = [
            subprocess.TimeoutExpired(
                ["ipsw", "extract"], 300, stderr=b"Extracting files"
            ),
            subprocess.CalledProcessError(
                -9, ["ipsw", "extract"], stderr=b"Extracting files"
            ),
        ]
        for error in errors:
            with self.subTest(error=type(error).__name__):
                with TemporaryDirectory() as temporary:
                    manifest = Path(temporary) / "manifest.json"
                    manifest.write_text(json.dumps([{"build": "25G83"}]))
                    arguments = ["fetch_dock_binaries.py"]
                    for name in ("ipsw", "parser", "recovery", "output"):
                        arguments.extend(["--" + name, temporary])
                    arguments.extend(["--manifest", str(manifest)])
                    output = io.StringIO()
                    with (
                        patch.object(sys, "argv", arguments),
                        patch.object(
                            fetcher.runpy,
                            "run_path",
                            return_value={
                                "intel_slice": Mock(),
                                "mount_recovery": Mock(),
                            },
                        ),
                        patch.object(fetcher, "fetch", side_effect=error),
                        redirect_stdout(output),
                    ):
                        self.assertEqual(fetcher.main(), 1)
                    self.assertIn(str(error), output.getvalue())
                    self.assertIn("Extracting files", output.getvalue())

    def test_tar_failure_preserves_its_error_message(self):
        tar = Mock()
        tar.stderr = io.BytesIO(b"tar: invalid payload")
        tar.wait.return_value = 1
        process = Mock()
        process.__enter__ = Mock(return_value=tar)
        process.__exit__ = Mock(return_value=False)
        payload = io.BytesIO(b"pbzx" + bytes(8))
        with TemporaryDirectory() as temporary:
            with patch.object(
                fetcher.subprocess, "Popen", return_value=process
            ):
                with self.assertRaisesRegex(ValueError, "invalid payload"):
                    fetcher.extract_payload(
                        payload, 0, 12, Path(temporary) / "Dock", "unused"
                    )


if __name__ == "__main__":
    unittest.main()
