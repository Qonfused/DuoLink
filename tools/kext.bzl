## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Compile Intel kernel extensions and assemble standalone or nested bundles."""

load("@developer_tools//:toolchain.bzl", "CLANG", "SDK")

def _quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"

def _kext_impl(ctx):
    version = ctx.var.get("duolink_version", ctx.attr.version)
    kernel_root = ctx.file.kernel_anchor.dirname.rsplit("/", 1)[0]
    executable = ctx.label.name
    common = [
        "-arch",
        "x86_64",
        "-std=c++14",
        "-isysroot",
        SDK,
        "-mmacosx-version-min=10.11",
        "-mkernel",
        "-fapple-kext",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-builtin",
        "-fno-common",
        "-fno-stack-protector",
        "-fno-asynchronous-unwind-tables",
        "-nostdinc++",
        "-O0" if ctx.attr.debug else "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-Wno-deprecated-register",
        "-Wno-unknown-warning-option",
        "-Wno-ossharedptr-misuse",
        "-DKERNEL=1",
        "-DKERNEL_PRIVATE=1",
        '-DKEXT_VERSION="' + version + '"',
        "-isystem",
        kernel_root,
        "-I",
        ".",
    ]
    if ctx.attr.debug:
        common += ["-DDEBUG=1"]
    inputs = ctx.files.hdrs + ctx.files.kernel_headers + [ctx.file._fingerprint]
    if ctx.attr.lilu:
        lilu_root = ctx.file._lilu_anchor.dirname
        common += [
            "-mno-red-zone",
            "-mno-sse",
            "-mno-sse2",
            "-mno-mmx",
            "-DLILU_KEXTPATCH_SUPPORT=1",
            "-DPRODUCT_NAME=" + executable,
            "-DMODULE_VERSION=" + version,
            "-DLILU_CUSTOM_IOKIT_INIT=1",
            "-isystem",
            lilu_root + "/..",
        ]
        inputs += ctx.files._lilu_headers
    else:
        common += ["-nostdinc", "-DDRIVER_PRIVATE=1", "-DAPPLE=1", "-DNeXT=1"]
    for anchor in ctx.files.header_anchors:
        common += ["-isystem", anchor.dirname]
    inputs += ctx.files.header_anchors
    objects = []
    for index, source in enumerate(ctx.files.srcs):
        obj = ctx.actions.declare_file(ctx.label.name + ".objects/%d.o" % index)
        args = ctx.actions.args()
        args.add_all(common + ["-c", source.path, "-o", obj.path])
        ctx.actions.run(
            executable = CLANG,
            arguments = [args],
            inputs = inputs + [source],
            outputs = [obj],
            mnemonic = "CompileKernelExtension",
            execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
        )
        objects.append(obj)
    if ctx.files.c_srcs:
        c_flags = [f for f in common if f not in (
            "-std=c++14",
            "-fapple-kext",
            "-fno-exceptions",
            "-fno-rtti",
            "-nostdinc++",
        )] + [
            "-x",
            "c",
            "-std=c99",
            "-fvisibility=hidden",
        ] + ["-D" + value for value in ctx.attr.c_defines]
        for anchor in ctx.files.c_header_anchors:
            c_flags += ["-isystem", anchor.dirname]
        for index, source in enumerate(ctx.files.c_srcs):
            obj = ctx.actions.declare_file(ctx.label.name + ".c_objects/%d.o" % index)
            args = ctx.actions.args()
            args.add_all(c_flags + ["-c", source.path, "-o", obj.path])
            ctx.actions.run(
                executable = CLANG,
                arguments = [args],
                inputs = inputs + ctx.files.c_hdrs + ctx.files.c_header_anchors + [source],
                outputs = [obj],
                mnemonic = "CompileKernelC",
                execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
            )
            objects.append(obj)
    binary = ctx.actions.declare_file(ctx.label.name + ".bin")
    args = ctx.actions.args()
    args.add_all([
        "-arch",
        "x86_64",
        "-isysroot",
        SDK,
        "-mmacosx-version-min=10.11",
        "-nostdlib",
        "-static",
        "-Wl,-kext",
        "-Wl,-no_deduplicate",
    ])
    args.add_all(objects)
    args.add(ctx.file.libkmod)
    args.add_all(["-o", binary.path])
    ctx.actions.run(
        executable = CLANG,
        arguments = [args],
        inputs = objects + [ctx.file.libkmod, ctx.file._fingerprint],
        outputs = [binary],
        mnemonic = "LinkKernelExtension",
        execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
    )
    bundle = ctx.actions.declare_directory(ctx.label.name + ".kext")
    commands = [
        "set -eu",
        "mkdir -p %s/Contents/MacOS" % _quote(bundle.path),
        "cp %s %s/Contents/MacOS/%s" % (_quote(binary.path), _quote(bundle.path), _quote(executable)),
        "/usr/bin/python3 %s %s %s/Contents/Info.plist %s %s" % (
            _quote(ctx.file._plist_processor.path),
            _quote(ctx.file.infoplist.path),
            _quote(bundle.path),
            _quote(executable),
            _quote(version),
        ),
        "mkdir -p %s/Contents/Resources/Licenses" % _quote(bundle.path),
        "cp %s %s/Contents/Resources/Licenses/%s.txt" % (_quote(ctx.file.license.path), _quote(bundle.path), _quote(executable)),
        "cp %s %s/Contents/Resources/Licenses/MacKernelSDK.txt" % (_quote(ctx.file.kernel_license.path), _quote(bundle.path)),
        "/usr/bin/plutil -lint %s/Contents/Info.plist" % _quote(bundle.path),
    ]
    bundle_inputs = []
    for target, name in ctx.attr.extra_licenses.items():
        license = target.files.to_list()[0]
        commands += ["cp %s %s/Contents/Resources/Licenses/%s" % (_quote(license.path), _quote(bundle.path), _quote(name))]
        bundle_inputs.append(license)
    bundle_inputs += [
        binary,
        ctx.file.infoplist,
        ctx.file.license,
        ctx.file.kernel_license,
        ctx.file._plist_processor,
    ] + ctx.files.plugins
    if ctx.attr.lilu:
        commands += ["cp %s %s/Contents/Resources/Licenses/Lilu.txt" % (_quote(ctx.file._lilu_license.path), _quote(bundle.path))]
        bundle_inputs += [ctx.file._lilu_license]
    if ctx.files.plugins:
        commands += ["mkdir -p %s/Contents/PlugIns" % _quote(bundle.path)]
        for plugin in ctx.files.plugins:
            commands += ["cp -R %s %s/Contents/PlugIns/" % (_quote(plugin.path), _quote(bundle.path))]
    ctx.actions.run_shell(
        inputs = bundle_inputs,
        outputs = [bundle],
        command = "\n".join(commands),
        mnemonic = "BundleKernelExtension",
        execution_requirements = {"requires-darwin": "1", "no-remote": "1"},
    )
    return [DefaultInfo(files = depset([bundle]))]

kext = rule(
    implementation = _kext_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".cpp"]),
        "c_srcs": attr.label_list(allow_files = [".c"]),
        "c_defines": attr.string_list(),
        "c_hdrs": attr.label_list(allow_files = True),
        "c_header_anchors": attr.label_list(allow_files = True),
        "extra_licenses": attr.label_keyed_string_dict(allow_files = True),
        "hdrs": attr.label_list(allow_files = True),
        "infoplist": attr.label(allow_single_file = True, mandatory = True),
        "_plist_processor": attr.label(default = "//tools:process_info.py", allow_single_file = True),
        "lilu": attr.bool(default = True),
        "debug": attr.bool(default = False),
        "version": attr.string(mandatory = True),
        "plugins": attr.label_list(allow_files = True),
        "header_anchors": attr.label_list(allow_files = True),
        "kernel_headers": attr.label(default = "@mac_kernel_sdk//:headers"),
        "kernel_anchor": attr.label(default = "@mac_kernel_sdk//:Headers/mach/kmod.h", allow_single_file = True),
        "libkmod": attr.label(default = "@mac_kernel_sdk//:Library/x86_64/libkmod.a", allow_single_file = True),
        "kernel_license": attr.label(default = "@mac_kernel_sdk//:LICENSE.txt", allow_single_file = True),
        "license": attr.label(default = "//:LICENSE", allow_single_file = True),
        "_lilu_headers": attr.label(default = "@lilu_sdk//:headers"),
        "_lilu_anchor": attr.label(default = "@lilu_sdk//:Lilu/Headers/kern_api.hpp", allow_single_file = True),
        "_fingerprint": attr.label(default = "@developer_tools//:fingerprint.txt", allow_single_file = True),
        "_lilu_license": attr.label(default = "@lilu_sdk//:LICENSE.txt", allow_single_file = True),
    },
)
