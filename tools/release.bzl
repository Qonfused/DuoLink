## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Package DuoLink for GitHub Releases and OpenCore dependency managers."""

def _release_archive_impl(ctx):
    version = ctx.var.get("duolink_version", ctx.attr.version)
    basename = "DuoLink-%s-%s.zip" % (version, ctx.attr.build_type)
    archive = ctx.actions.declare_file(basename)
    checksum = ctx.actions.declare_file(basename + ".sha256")
    args = ctx.actions.args()
    args.add_all([
        ctx.file._packager.path,
        "--bundle",
        ctx.file.bundle.path,
        "--version",
        version,
        "--build-type",
        ctx.attr.build_type,
        "--output",
        archive.path,
        "--checksum",
        checksum.path,
    ])
    for doc in ctx.files.docs:
        args.add_all(["--document", doc.short_path + "=" + doc.path])
    ctx.actions.run(
        executable = "/usr/bin/python3",
        arguments = [args],
        inputs = [ctx.file.bundle, ctx.file._packager] + ctx.files.docs,
        outputs = [archive, checksum],
        mnemonic = "PackageDuoLink",
    )
    return [DefaultInfo(files = depset([archive, checksum]))]

release_archive = rule(
    implementation = _release_archive_impl,
    attrs = {
        "bundle": attr.label(allow_single_file = True, mandatory = True),
        "docs": attr.label_list(allow_files = True),
        "version": attr.string(mandatory = True),
        "build_type": attr.string(values = ["DEBUG", "RELEASE"], mandatory = True),
        "_packager": attr.label(default = "//tools:package_release.py", allow_single_file = True),
    },
)
