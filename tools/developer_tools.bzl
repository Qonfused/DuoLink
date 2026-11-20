## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Locate and fingerprint the Apple compiler used for local macOS builds."""

def _run(ctx, args):
    result = ctx.execute(args)
    if result.return_code:
        fail("Apple developer tools are unavailable:\n" + result.stderr)
    return result.stdout.strip()

def _developer_tools_impl(ctx):
    if ctx.os.name != "mac os x":
        fail("DuoLink currently builds on macOS with Apple developer tools.")
    clang = _run(ctx, ["/usr/bin/xcrun", "--find", "clang++"])
    sdk = _run(ctx, ["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"])
    version = _run(ctx, [clang, "--version"])
    settings = sdk + "/SDKSettings.plist"
    ctx.watch(clang)
    ctx.watch(settings)
    ctx.file("toolchain.bzl", "CLANG = %r\nSDK = %r\n" % (clang, sdk))
    ctx.file("fingerprint.txt", version + "\n" + sdk + "\n" + ctx.read(settings))
    ctx.file("BUILD.bazel", 'exports_files(["toolchain.bzl", "fingerprint.txt"])\n')

developer_tools = repository_rule(
    implementation = _developer_tools_impl,
    local = True,
    configure = True,
    environ = ["DEVELOPER_DIR"],
)
