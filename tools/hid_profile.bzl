## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Describe the ABI of the supported stock input executables."""

def _hid_profile_impl(ctx):
    output = ctx.actions.declare_file("stock_input_profile.hpp")
    ctx.actions.run(
        executable = "/usr/bin/python3",
        arguments = [
            ctx.file._generator.path,
            ctx.file.core.path,
            ctx.file.hid.path,
            ctx.file.dsym.path,
            output.path,
        ],
        inputs = [ctx.file.core, ctx.file.hid, ctx.file.dsym, ctx.file._generator],
        outputs = [output],
        mnemonic = "VerifyStockInputProfile",
        execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
    )
    return [DefaultInfo(files = depset([output]))]

hid_profile = rule(
    implementation = _hid_profile_impl,
    attrs = {
        "core": attr.label(mandatory = True, allow_single_file = True),
        "hid": attr.label(mandatory = True, allow_single_file = True),
        "dsym": attr.label(mandatory = True, allow_single_file = True),
        "_generator": attr.label(
            default = "//tools:generate_hid_profile.py",
            allow_single_file = True,
        ),
    },
)
