#!/usr/bin/python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Resolve nested debug members independently of their source line numbers."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
from unittest.mock import patch

root = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
spec = importlib.util.spec_from_file_location(
    "hid_profile", root / "tools/generate_hid_profile.py"
)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

fixture = r"""
0x0001: DW_TAG_structure_type
              DW_AT_name ("ValueWithTime")
              DW_AT_byte_size (0x10)
0x0002:   DW_TAG_member
                DW_AT_name ("timestamp")
                DW_AT_decl_line (9999)
                DW_AT_data_member_location (0x08)
0x0003:   DW_TAG_structure_type
                DW_AT_name ("Nested")
                DW_AT_byte_size (0x20)
0x0004:     DW_TAG_member
                  DW_AT_name ("timestamp")
                  DW_AT_decl_line (24)
                  DW_AT_data_member_location (0x18)
0x0005:     NULL
0x0006:   NULL
0x0007: DW_TAG_structure_type
              DW_AT_name ("OtherType")
              DW_AT_byte_size (0x20)
0x0008:   DW_TAG_member
                DW_AT_name ("timestamp")
                DW_AT_decl_line (24)
                DW_AT_data_member_location (0x10)
0x0009:   NULL
"""
with patch.object(module.subprocess, "check_output", return_value=fixture):
    debug = module.DebugInfo("unused")
    assert debug.member("ValueWithTime", "timestamp") == 8
    assert debug.size("ValueWithTime") == 16
    assert debug.member("OtherType", "timestamp") == 16
    try:
        debug.member("ValueWithTime", "absent")
    except ValueError:
        pass
    else:
        raise AssertionError("Missing member accepted")

for data in [
    bytes(32),
    struct.pack("<8I", 0xFEEDFACF, 0x01000007, 0, 0xB, 1, 8, 0, 0)
    + struct.pack("<2I", 0x19, 8),
]:
    try:
        module.image(data)
    except ValueError:
        pass
    else:
        raise AssertionError("Malformed image accepted")

print("Member ownership and malformed binary checks passed.")
