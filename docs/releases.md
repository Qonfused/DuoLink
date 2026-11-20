# Builds and releases

## Build packages locally

The release target creates a ZIP containing `DuoLink.kext`, its nested
DuoLinkTouch companion, licenses, and the public documentation. Build each
configuration from the repository root:

```sh
./bazelw build --config=release //:release
./bazelw build --config=debug //:release
```

| Configuration | Archive | Compiler flags |
| --- | --- | --- |
| `release` | `DuoLink-<version>-RELEASE.zip` | `-O2` |
| `debug` | `DuoLink-<version>-DEBUG.zip` | `-O0 -DDEBUG=1` |

DEBUG builds retain Lilu startup diagnostics; add `-duolinkdbg` when booting
one to enable the extra logging.

Bazel writes the archives and their `.sha256` files under `bazel-bin/`.
[versions.bzl](../versions.bzl) supplies the defaults for local and branch
builds. Release builds take their version from the Git tag and stamp both
extensions, their executables, and the archive with that version. Each
archive includes `build.json` with its bundle versions, build configuration,
and SHA-256 hashes of its files.

DuoLink identifies CoreDisplay and Dock instructions on the system where it
loads. Release jobs therefore compile and package the same sources as local
builds, without requiring a mounted target macOS installation.
[Runtime CoreDisplay matching](displays-and-dock.md#matching-coredisplay-at-runtime)

## GitHub Actions

The [Build workflow](../.github/workflows/build.yml) runs on pull requests,
pushes to `main`, and manual dispatch. Formatting, workflow, and documentation
link checks must pass before either build starts. Both configurations run
the full test suite, including the ASSR and Dock binary checks, before
packaging. Their
archives and checksums are uploaded as separate `DuoLink-RELEASE` and
`DuoLink-DEBUG` workflow artifacts. Download and unpack a workflow artifact to
obtain its versioned ZIP.

Push a version tag such as `v0.4.6` to trigger the
[Release workflow](../.github/workflows/release.yml). The shared
[Build workflow](../.github/workflows/build.yml) reads the tag and passes
`--define=duolink_version=0.4.6` to Bazel for both configurations. No edit to
`versions.bzl` or either plist is required.

After both configurations pass their tests, the release workflow checks the
archive names and checksums, then publishes the two ZIPs with a combined
`SHA256SUMS` file. To reproduce a tagged build locally, pass the same version
flag:

```sh
./bazelw build --config=release --define=duolink_version=0.4.6 //:release
```

## OCE-Build

[OCE-Build's GitHub resolver](https://github.com/Qonfused/OCE-Build/blob/main/ocebuild/sources/resolver.py#L108-L177)
selects release assets by bundle name and `RELEASE` or `DEBUG` build type.
These archive names let it select the requested configuration without a
custom URL. Its [kext extractor](https://github.com/Qonfused/OCE-Build/blob/main/ocebuild/pipeline/kexts.py#L262-L284)
finds DuoLinkTouch inside DuoLink, so the companion stays bundled.

Once a release is published, an EFI project's `build.yml` can use:

```yaml
Kexts:
  DuoLink:
    specifier: Qonfused/DuoLink=latest
    bundled:
      - DuoLinkTouch
    properties:
      Arch: x86_64
  DuoLinkTouch:
    specifier: "*"
    properties:
      Arch: x86_64
```

OCE-Build's `build` setting chooses the release asset. The companion's
wildcard entry uses the copy inside DuoLink instead of resolving a second
download. Keep Lilu and the stock input drivers in the EFI build as described
in [Installation](installation.md).
