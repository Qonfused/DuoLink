## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Extract checksum-pinned Dock fixtures from Apple's public installers."""

def _dock_binaries_impl(ctx):
    fetcher = ctx.path(ctx.attr.fetcher)
    recovery = ctx.path(ctx.attr.recovery)
    parser = ctx.path(ctx.attr.parser)
    manifest = ctx.path(ctx.attr.manifest)
    for source in (fetcher, recovery, parser, manifest):
        ctx.watch(source)
    cache = ctx.path(ctx.getenv(
        "DUOLINK_DOCK_CACHE",
        str(fetcher.dirname.dirname.get_child(".cache", "dock")),
    ))

    # Bazel requires a positive timeout. macOS accepts at most 100000000
    # seconds; use that limit and leave cancellation to the caller.
    result = ctx.execute([
        "/usr/bin/python3",
        fetcher,
        "--ipsw",
        ctx.path(ctx.attr.ipsw),
        "--parser",
        parser,
        "--recovery",
        recovery,
        "--manifest",
        manifest,
        "--output",
        cache,
    ], timeout = 100000000)
    if result.return_code:
        fail("Dock fixture setup failed:\n" + result.stdout + result.stderr)
    print(result.stdout.strip())
    releases = json.decode(ctx.read(manifest))
    paths = []
    for release in releases:
        source = cache.get_child(release["build"], "Dock")
        ctx.watch(source)
        destination = "binaries/" + release["build"] + "/Dock"
        ctx.symlink(source, destination)
        paths.append(destination)
    ctx.file("BUILD.bazel", """
filegroup(
    name = "binaries",
    srcs = %s,
    visibility = ["//visibility:public"],
)
""" % repr(paths))

dock_binaries = repository_rule(
    implementation = _dock_binaries_impl,
    environ = ["DUOLINK_DOCK_CACHE"],
    attrs = {
        "fetcher": attr.label(default = "//tools:fetch_dock_binaries.py", allow_single_file = True),
        "recovery": attr.label(default = "//tools:extract_recovery_kext.py", allow_single_file = True),
        "parser": attr.label(default = "//tools:macho.py", allow_single_file = True),
        "manifest": attr.label(default = "//tests/compatibility:dock_binaries.json", allow_single_file = True),
        "ipsw": attr.label(mandatory = True, allow_single_file = True),
    },
)
