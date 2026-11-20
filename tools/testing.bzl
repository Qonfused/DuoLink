## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Run C++ and Python tests on macOS."""

load("@developer_tools//:toolchain.bzl", "CLANG", "SDK")

def _host_test_impl(ctx):
    executable = ctx.actions.declare_file(ctx.label.name)
    args = ctx.actions.args()
    args.add_all([
        "-std=c++14",
        "-isysroot",
        SDK,
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1",
        "-I",
        ".",
    ])
    args.add(ctx.file.src)
    args.add_all(["-o", executable.path])
    ctx.actions.run(
        executable = CLANG,
        arguments = [args],
        inputs = [ctx.file.src, ctx.file._fingerprint] + ctx.files.hdrs,
        outputs = [executable],
        execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
        mnemonic = "CompileDuoLinkTest",
    )
    return [DefaultInfo(executable = executable)]

host_test = rule(
    implementation = _host_test_impl,
    test = True,
    attrs = {
        "hdrs": attr.label_list(allow_files = True),
        "src": attr.label(allow_single_file = [".cpp"], mandatory = True),
        "_fingerprint": attr.label(default = "@developer_tools//:fingerprint.txt", allow_single_file = True),
    },
)

def _script_test_impl(ctx):
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.expand_template(
        template = ctx.file.src,
        output = executable,
        substitutions = {},
        is_executable = True,
    )
    return [DefaultInfo(executable = executable, runfiles = ctx.runfiles(files = ctx.files.data))]

script_test = rule(
    implementation = _script_test_impl,
    test = True,
    attrs = {
        "src": attr.label(allow_single_file = True, mandatory = True),
        "data": attr.label_list(allow_files = True),
    },
)
