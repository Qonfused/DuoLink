## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Generate patterns and operand offsets for the CoreDisplay matcher."""

load("@developer_tools//:toolchain.bzl", "CLANG")

def _display_patch_impl(ctx):
    header = ctx.actions.declare_file("display_patch_data.hpp")
    args = ctx.actions.args()
    args.add_all([
        ctx.file._generator.path,
        "--clang",
        CLANG,
        "--template",
        ctx.file.template.path,
        "--header",
        header.path,
    ])
    ctx.actions.run(
        executable = "/usr/bin/python3",
        arguments = [args],
        inputs = [
            ctx.file._generator,
            ctx.file._macho,
            ctx.file._fingerprint,
            ctx.file.template,
        ],
        outputs = [header],
        mnemonic = "AssembleDisplayPatch",
        execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
    )
    return [DefaultInfo(files = depset([header]))]

display_patch = rule(
    implementation = _display_patch_impl,
    attrs = {
        "template": attr.label(allow_single_file = True, mandatory = True),
        "_generator": attr.label(default = "//tools:generate_display_patch.py", allow_single_file = True),
        "_macho": attr.label(default = "//tools:macho.py", allow_single_file = True),
        "_fingerprint": attr.label(default = "@developer_tools//:fingerprint.txt", allow_single_file = True),
    },
)
