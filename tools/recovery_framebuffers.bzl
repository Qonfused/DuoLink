## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Fetch and cache Intel framebuffer drivers from Apple Recovery."""

_RECOVERIES = [
    ["Catalina", "19H2", "Mac-00BE6ED71E35EB86", "default"],
    ["Big Sur", "20G165", "Mac-2BD1B31983FE1663", "default"],
    ["Monterey", "21G83", "Mac-E43C1C25D4880AD6", "default"],
    ["Ventura", "22F82", "Mac-B4831CEBD52A0C4C", "default"],
    ["Sonoma", "23G93", "Mac-827FAC58A8FDFA22", "default"],
    ["Sequoia", "24E263", "Mac-7BA5B2D9E42DDD94", "default"],
    ["Tahoe", "25G83", "Mac-CFF7D910A743CAAF", "latest"],
]

def _recovery_framebuffers_impl(ctx):
    fetcher = ctx.path(ctx.attr.fetcher)
    extractor = ctx.path(ctx.attr.extractor)
    ctx.watch(fetcher)
    ctx.watch(extractor)

    # Bazel requires a positive timeout. macOS accepts at most 100000000
    # seconds; use that limit and leave cancellation to the caller.
    result = ctx.execute([
        "/usr/bin/python3",
        fetcher,
        extractor,
        ctx.path(ctx.attr.macrecovery),
        ctx.path(ctx.attr.ipsw),
        ctx.path("kexts"),
        json.encode(_RECOVERIES),
    ], timeout = 100000000)
    if result.return_code:
        fail("Recovery framebuffer setup failed\n" + result.stdout + result.stderr)
    print(result.stdout.strip())

    paths = [
        "kexts/" + build + "/AppleIntelCFLGraphicsFramebuffer"
        for _, build, _, _ in _RECOVERIES
    ]
    ctx.file("BUILD.bazel", """
filegroup(
    name = "framebuffers",
    srcs = %s,
    visibility = ["//visibility:public"],
)
""" % repr(paths))

recovery_framebuffers = repository_rule(
    implementation = _recovery_framebuffers_impl,
    attrs = {
        "extractor": attr.label(default = "//tools:extract_recovery_kext.py", allow_single_file = True),
        "fetcher": attr.label(default = "//tools:fetch_recovery_kexts.py", allow_single_file = True),
        "ipsw": attr.label(mandatory = True, allow_single_file = True),
        "macrecovery": attr.label(mandatory = True, allow_single_file = True),
    },
)
