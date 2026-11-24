# Development

## Build and test

Run from the repository root on an Intel or Apple silicon Mac with Xcode or
Command Line Tools:

```sh
./bazelw build //:DuoLink
./bazelw test //tests:all
```

Bazel fetches Lilu source, kernel SDKs, and the official VoodooI2C release
pinned in [MODULE.bazel](../MODULE.bazel). Kernel extensions compile for Intel;
host tools and test programs run on the build Mac's CPU without Rosetta.

| Output | Purpose |
| --- | --- |
| `bazel-bin/DuoLink.kext` | Lilu plugin with DuoLinkTouch bundled in `Contents/PlugIns`. |
| `bazel-bin/DuoLinkTouch.kext` | Companion extension built separately before bundling. |
| `bazel-bin/display_patch_data.hpp` | Recognized CoreDisplay size checks, assembled replacements, and stack operand positions. |
| `bazel-bin/stock_input_profile.hpp` | VoodooI2C and VoodooI2CHID method entries, field offsets, and virtual method slots checked against the pinned release. |

Bazel assembles the CoreDisplay size check for each supported register layout.
DuoLink locates the mode check and reads its stack operands at runtime, so
builds require no target volume or dyld cache.
[CoreDisplay matching](displays-and-dock.md#matching-coredisplay-at-runtime)

## Binary compatibility tests

The default test suite, `//tests:all`, includes ScreenPad link-training and Dock
checks against pinned Apple binaries. Run just those checks with:

```sh
./bazelw test //tests:compatibility --test_output=all
```

### ScreenPad link training

The [Recovery manifest](../tools/recovery_framebuffers.bzl) pins seven builds
from Catalina through Tahoe. On the first run, Bazel extracts their Intel
framebuffer drivers in parallel and retains them in its dependency cache.
Tests resolve the four driver symbols used by DuoLink's ASSR fix and check
whether the opening instructions of `WriteAUX` can be copied into a hook
trampoline. They use the same prologue check as DuoLink before route
installation. [Symbol and prologue checks](../tests/compatibility/assr_test.py)

```sh
./bazelw test //tests/compatibility:assr_test --test_output=all
```

### Check Dock binaries

The [Dock manifest](../tests/compatibility/dock_binaries.json) pins eight builds
from Catalina through Tahoe, including Ventura 13.1 and 13.4.1, with an Apple
installer URL and SHA-256 for each Intel executable.

```sh
./bazelw test //tests/compatibility:dock_test --test_output=all
```

For each executable, the test requires DuoLink's matcher to identify exactly
one placement site. It patches a copy of that file page, checks that only
`0x75` changes to `0xEB`, and disassembles both versions to confirm that the
jump still reaches Dock's original fallback. Repeated application must leave
the page unchanged; inconsistent branch targets and multiple matches must be
rejected. [Binary checks](../tests/compatibility/dock_test.py) ·
[Production matcher checks](../tests/compatibility/check_dock.cpp)

On the first run, Bazel builds a pinned revision of `ipsw` with its own Go
toolchain and extracts Dock using byte-range reads from Apple's restore
images. Catalina uses its installer package, mounted read-only while the
extractor streams `Core.pkg` until Dock is complete. Each Intel executable
must match its pinned checksum before it is cached or tested. Fixtures stay
in `.cache/dock`; set `--repo_env=DUOLINK_DOCK_CACHE=/absolute/path` to use
another directory. Go module and build caches also stay under `.cache`.
[Tool build](../tools/ipsw_ranged.bzl) ·
[Fixture extraction](../tools/fetch_dock_binaries.py)

For executables with protected code, the tests decode a temporary copy using
[class-dump's Blowfish implementation](https://github.com/nygard/class-dump/blob/2c82b4ff12b2ea5d1ac23e49281d496997370841/ThirdParty/blowfish.c).
That decoder dependency retains its LGPL-2.1-or-later license and is used only
by tests; it is not linked into DuoLink's kernel extensions.

Pass `--test_arg=BUILD=/absolute/path/to/Dock` to check another original
executable alongside the eight pinned builds. Reading Dock from a system
running DuoLink can return bytes that already contain the placement patch,
so use an unmodified copy.

These tests check symbol resolution, instruction layouts, and patch results.
After changing runtime hooks, also check boot, panel power cycling, display
arrangement, and sleep/wake on the laptop using the
[display](displays-and-dock.md) and [input](input-mapping.md#checking-input)
guides. Record the macOS build with those results.

## CoreDisplay matching tests

[CoreDisplay captures](../tests/fixtures/core_display.json) preserve the
original and validated replacement instructions from macOS 13.1 (`22C65`)
and 13.7.8 (`22H730`). The latter also includes the surrounding executable
page, recorded with its cache file offset and the mode check's position.
Build identifiers describe those captures; runtime selection uses the
instruction layouts and operands.

```sh
./bazelw test //tests:display_patch_test --test_output=all
```

The matcher must produce the same instructions as the assembler when stack
operands move. The page checks reject stale or ambiguous matches and verify
that the write ends before CoreDisplay tests the driver's valid bit.
[Fixture and relocation checks](../tests/display_patch_test.py)

CI runs this test against every 4 KiB page in the Intel build matrix. A build
passes when DuoLink patches the block containing the recorded dimension
check and recognizes the replacement on a second pass. Eight pages came from
Intel Recovery frameworks and three from desktop shared caches. Recovery
and desktop code can differ even within one release.
[Builds, sources, offsets, and checksums](../tests/fixtures/core_display_pages/manifest.json)

For a new build, run `python3 tools/inspect_display_mode.py /path/to/CoreDisplay`
on a symbol-bearing Intel framework. It locates the size decision in
`description_for_mode_number` and prints the instruction's address and
offset within its page. Review the instructions and both branch destinations
before capturing the page. For a desktop cache, take the original 4096 bytes
at that virtual page address from the cache so the branch guard sees the same
neighboring instructions as it would at runtime. For a packed comparison,
capture its 16-byte threshold vector as well. The Intel execution test
relinks the instruction to those bytes when running outside the shared cache.
[CoreDisplay inspector](../tools/inspect_display_mode.py)

Across the sampled builds, CoreDisplay exempts television modes before
testing desktop dimensions. Catalina and Big Sur branch on signed width and
height comparisons. Sequoia combines the comparison results in byte
registers, while Sonoma tests two lanes against the constants 799 and 599.
Tahoe uses scalar branches again and loads a live register between its
comparison and conditional jump. Matching only the numeric constants would
miss Sonoma and could select an unrelated mode check elsewhere.

On Intel, the test runs each distinct pair of original and patched instruction
blocks from the supported captures. The two recorded Ventura replacements
also serve as references for those layouts. A native 1920×515 mode must receive
the classification Apple gives 1920×600 with the same pixel format and
driver flags. For every other mode the two blocks must agree at the original
dimensions. Each layout uses its own register assignments to return the
classification and the values CoreDisplay needs after the check.
[Mode comparisons](../tests/display_patch_test.py) ·
[Intel register setup](../tests/compatibility/display_modes.S)

Incoming direct jumps and calls into the replacement must leave the page
unchanged, including when they follow a return or inline data. Decoder cases
cover SIMD instructions and truncated input. The tests compile the same
stateless decoder and adapter as the kernel extension; DuoLink's linked
decoder objects must import no allocator or shared Capstone API.
[Page and branch checks](../tests/compatibility/check_display.cpp) ·
[Bundle link checks](../tests/bundle_test.py)

## Inspect framebuffer drivers from Recovery

To inspect a build outside the automated tests, run the extractor on macOS
with OpenCore's `macrecovery.py` and an Intel board ID. Install `ipsw` for
Recovery images that store the framebuffer in a kernel collection.
[Recovery board IDs](https://github.com/acidanthera/OpenCorePkg/blob/master/Utilities/macrecovery/recovery_urls.txt) ·
[ipsw releases](https://github.com/blacktop/ipsw/releases)

```sh
python3 tools/extract_recovery_kext.py \
  --macrecovery /path/to/macrecovery.py \
  --board-id Mac-E43C1C25D4880AD6
```

The extractor mounts Recovery read-only, fetches the requested image chunks,
and checks them against Apple's signed chunklist. It saves the Intel
framebuffer executable under `.cache/recovery-kexts/<build>/`. Pass `--build`
with a board ID to require an exact build, or pass `--build` alone to reuse a
cached executable without contacting Apple. Apple can change which build it
serves for a board ID.

## Source organization

| Location | Responsibility |
| --- | --- |
| `src/kern_duolink.cpp` | Lilu startup and feature registration. |
| `src/screenpad/` | Match the panel's ASSR setting to the GPU during link training. |
| `src/display/`, `src/dock/` | Allow the native ScreenPad mode in Displays and keep Dock on the primary display. |
| `src/input/` | Recognize stock drivers, install native method hooks, and read fields using verified offsets. |
| `src/touch/duolink_touch.cpp` | Start the companion and register IOService matching notifications. |
| `src/touch/touchscreen.cpp` | Select each panel's framebuffer and position its cursor before native input dispatch. |
| `src/touch/touchpad.cpp` | Request ELAN1207 touch-surface and button reporting after native mode selection. |
| `src/diagnostics/` | Publish deferred IORegistry updates and optionally observe raw input packets. |
| `tools/` | Compile and package extensions, fetch test binaries, and generate driver metadata and display patch templates. |
| `tests/` | Check patch behavior, display mapping, build tools, and kext packaging. |

## Callback and object lifetimes

Both extensions refuse unload because installed hooks can call DuoLink until
reboot. DuoLinkTouch therefore keeps its callback table and panel locks after
`DuoLinkTouch::stop` removes the matching notifications. Native driver start
and stop hooks can still attach and detach individual drivers through those
callbacks. [Callback interface](../src/input/routing_api.hpp) ·
[Companion lifecycle](../src/touch/duolink_touch.cpp)

Each `PanelState` borrows its driver pointer until the native stop hook clears
it and retains its selected framebuffer independently of VoodooI2CHID's
framebuffer cache. Cache invalidation clears the native field before
releasing its reference because release can trigger callbacks that read the
field again. `PanelGuard` holds a recursive lock across native report
handling, allowing those framebuffer and digitizer callbacks to reenter
without exposing partly updated panel state.
[Panel state](../src/touch/touchscreen.cpp) ·
[Native cache adapter](../src/input/native_hid.hpp)

DuoLink publishes IORegistry updates through a deferred kernel callback.
Service stop clears the callback pointer under the scheduler lock, then
waits for queued or running updates before releasing the callback. The lock
remains allocated so hooks called after service stop can safely check
whether an update can be scheduled.
[Diagnostic service](../src/diagnostics/diagnostics.cpp)

## Unit and packaging tests

| Behavior | Test |
| --- | --- |
| Enable ASSR only on ScreenPad Plus, preserve other configuration bits, and verify the write | [ASSR](../tests/assr_test.cpp) |
| Map touch coordinates inside the correct display, including at shared edges | [Touch mapping](../tests/touch_mapping_test.cpp) |
| Match DuoLinkTouch to the bundled DuoLink and keep Voodoo drivers external | [Kext bundles](../tests/bundle_test.py) |
| Read fields from the right debug type and reject malformed Mach-O headers | [HID profile parser](../tests/hid_profile_parser_test.py) |
| Compare CoreDisplay classifications and reject altered size checks, stale matches, and branches into the replacement | [CoreDisplay matching](../tests/display_patch_test.py) |
| Reject altered Dock fixtures and installer responses that ignore byte ranges | [Fixture integrity](../tests/dock_fixture_test.py) |

## Formatting and versions

Project source filenames use `snake_case`. C and C++ use
[.clang-format](../.clang-format), with four-space indentation and an 80-column
limit. Python follows the Black configuration in
[pyproject.toml](../pyproject.toml). The formatting tools used for this source
are clang-format 18.1.8 and Black 24.10.0, with
[Buildifier 10.2.0](https://github.com/bazel-contrib/buildtools/releases/tag/v10.2.0)
for Starlark.

```sh
rg --files src tests -g '*.cpp' -g '*.hpp' -g '*.h' -g '*.c' \
  | xargs clang-format --dry-run --Werror
python3 -m black --check tools tests
buildifier -mode=check BUILD.bazel MODULE.bazel versions.bzl \
  tools/*.bzl tests/BUILD.bazel tests/compatibility/BUILD.bazel third_party/*.bazel
python3 tools/check_docs.py
```

[versions.bzl](../versions.bzl) defines each extension's version. The build
passes it to C++ as `KEXT_VERSION` and expands `$(MODULE_VERSION)` in its plist,
keeping executable declarations and bundle metadata aligned. Change
`OSBundleCompatibleVersion` when changing the binary interface required by
dependent kexts; change the callback API version when changing the table
exchanged between DuoLink and DuoLinkTouch.

For versioned ZIPs and publishing from version tags, see
[Builds and releases](releases.md).

## Review before publishing

The [Build workflow](../.github/workflows/build.yml) checks formatting,
workflow syntax, and local documentation links before running either build
configuration. CI uses the formatter versions listed above and actionlint
1.7.12 for workflow checks. Inline local links must refer to files included
in Git, and Markdown heading fragments must exist in their target documents.
[Link checker](../tools/check_docs.py)

Writing review follows the purpose of each guide: installation instructions
must explain the required setup, while technical explanations must connect
behavior to code or captured evidence. Check those claims and their scope
against the current implementation; formatter and link checks cannot judge
whether an explanation is accurate or useful.
