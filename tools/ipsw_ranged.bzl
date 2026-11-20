## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Build a pinned ipsw revision that supports ranged filesystem extraction."""

_GO = {
    "x86_64": ("amd64", "b6f8e931e4ab497fcde6da8ae4499a58f9d258e55c8a80824a516eae5950a750"),
    "aarch64": ("arm64", "dd3908cf20959d798c1e06ebe5b72213f0505ce01373b40ce3110975a6cd99c2"),
}

def _ipsw_ranged_impl(ctx):
    if ctx.os.name != "mac os x" or ctx.os.arch not in _GO:
        fail("Dock fixture extraction requires Intel or Apple silicon macOS")
    arch, checksum = _GO[ctx.os.arch]
    cache = ctx.path(ctx.attr.cache_anchor).dirname.dirname.get_child(".cache")
    ctx.download_and_extract(
        url = "https://go.dev/dl/go1.27.3.darwin-%s.tar.gz" % arch,
        sha256 = checksum,
        output = "go",
        stripPrefix = "go",
    )
    ctx.download_and_extract(
        url = "https://codeload.github.com/blacktop/ipsw/tar.gz/674128a8533f20ba7b6076502aadc16defd82d99",
        sha256 = "a840f6c79fcc1ffa0ff275dd05f3c44e3f2059e7888f9b713bce709d4ca2a3c7",
        output = "source",
        type = "tar.gz",
        stripPrefix = "ipsw-674128a8533f20ba7b6076502aadc16defd82d99",
    )

    # Bazel requires a positive timeout. macOS accepts at most 100000000
    # seconds; use that limit and leave cancellation to the caller.
    result = ctx.execute(
        [
            str(ctx.path("go/bin/go")),
            "build",
            "-mod=readonly",
            "-trimpath",
            "-p",
            "2",
            "-ldflags=-s -w",
            "-o",
            str(ctx.path("ipsw")),
            "./cmd/ipsw",
        ],
        working_directory = str(ctx.path("source")),
        environment = {
            "GOTOOLCHAIN": "local",
            "GOCACHE": str(cache.get_child("go-build")),
            "GOMODCACHE": str(cache.get_child("go-mod")),
            "GOMAXPROCS": "2",
            "GOMEMLIMIT": "750MiB",
        },
        timeout = 100000000,
    )
    if result.return_code:
        fail("Building ipsw for Dock fixtures failed:\n" + result.stdout + result.stderr)
    ctx.file("BUILD.bazel", 'exports_files(["ipsw"])\n')

ipsw_ranged = repository_rule(
    implementation = _ipsw_ranged_impl,
    attrs = {
        "cache_anchor": attr.label(default = "//tools:ipsw_ranged.bzl", allow_single_file = True),
    },
)
