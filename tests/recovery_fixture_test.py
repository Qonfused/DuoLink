#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Release recovery mounts and their HTTP server after extraction."""

import importlib.util
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch


workspace = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
spec = importlib.util.spec_from_file_location(
    "recovery_extractor", workspace / "tools/extract_recovery_kext.py"
)
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)


class RecoveryFixtures(unittest.TestCase):
    def setUp(self):
        self.server = MagicMock()
        self.server.__enter__.return_value = self.server
        self.server.server_port = 1234
        self.thread = MagicMock()
        for target, name, value in [
            (extractor, "HTTPServer", self.server),
            (extractor, "Thread", self.thread),
            (Path, "is_mount", True),
        ]:
            patched = patch.object(target, name, return_value=value)
            patched.start()
            self.addCleanup(patched.stop)
        self.image = SimpleNamespace(size=1, error=None)
        self.directory = Path("fixture-recovery")

    def test_busy_mount_does_not_discard_successful_extraction(self):
        def hdiutil(command, **kwargs):
            self.server.shutdown.assert_not_called()
            if command[1] == "detach" and "-force" not in command:
                raise subprocess.CalledProcessError(16, command)

        with patch.object(extractor.subprocess, "run", side_effect=hdiutil):
            with extractor.mount_recovery(self.image, self.directory) as mount:
                self.assertEqual(mount, self.directory / "Recovery")
        self.server.shutdown.assert_called_once()
        self.thread.join.assert_called_once()

    def test_extraction_error_still_releases_mount_and_server(self):
        with patch.object(extractor.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "invalid framebuffer"):
                with extractor.mount_recovery(self.image, self.directory):
                    raise ValueError("invalid framebuffer")
        self.assertEqual(run.call_count, 2)
        self.server.shutdown.assert_called_once()
        self.thread.join.assert_called_once()

    def test_detach_error_still_stops_the_server(self):
        def hdiutil(command, **kwargs):
            if command[1] == "detach":
                raise subprocess.CalledProcessError(16, command)

        with patch.object(extractor.subprocess, "run", side_effect=hdiutil):
            with self.assertRaises(subprocess.CalledProcessError):
                with extractor.mount_recovery(self.image, self.directory):
                    pass
        self.server.shutdown.assert_called_once()
        self.thread.join.assert_called_once()


if __name__ == "__main__":
    unittest.main()
