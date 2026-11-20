# Arranging ScreenPad Plus below the main display

## ScreenPad missing from Displays

On an affected UX481FL, you can drag windows onto ScreenPad Plus while System
Settings → Displays lists only the main panel. Detect Displays leaves the
arrangement unchanged because the framebuffer already drives both panels.
CoreDisplay requires standard desktop modes to be at least 800×600, so it
rejects the BOE087F panel's 1920×515 mode on height. With no eligible default
mode, Displays leaves ScreenPad Plus out of the arrangement.

DuoLink allows a physical 1920×515 mode through the size check when the
framebuffer marks it as native, preserving CoreDisplay's pixel-format and
driver-validity checks. After rebooting, check that both panels appear in
Displays, then run `ioreg -l -r -c DuoLink` and look for
`DisplayStatus = "patch-applied"`. Use `-duolinkdisplayoff` to disable this
display-detection fix. [CoreDisplay mode check](../src/display/display_mode.s.in)

## Keeping the Dock on the main display

Dock.app makes two decisions that matter when ScreenPad Plus sits below the
main display. During initial placement of a bottom Dock, it looks just beyond
the main display's lower edge and adopts an adjacent screen if it finds one.
It can also switch displays later in response to the pointer. Disabling
display switching leaves the initial placement search active, so keeping
Dock on the upper panel requires both DuoLink's placement patch and the
preference below.

DuoLink changes one conditional jump to an unconditional jump, directing Dock
to its existing primary-display fallback. Before changing the opcode, the
matcher checks that query failure, an empty result, and a result on the
current display all reach that fallback. The same matcher handles different
builds, including protected code after macOS decodes it.
[Placement matcher](../src/dock/dock_patch.hpp) ·
[Page hooks](../src/dock/kern_dock.cpp)

To prevent later display switching, run this once as your signed-in user:

```sh
defaults write com.apple.dock allow-display-switching -bool false
```

The preference applies to every display and takes effect when Dock next
starts. After rebooting, arrange the panels, check that Dock appears on the
main display, and look for `DockStatus = "patch-applied"` in
`ioreg -l -r -c DuoLink`. Repeat after a Dock restart, a display arrangement
change, and sleep/wake. Catalina lacks this preference, so later display
switching there remains unresolved.

Use `-duolinkdockoff` to disable the placement patch. To restore the native
switching preference, run `defaults delete com.apple.dock allow-display-switching`
and restart Dock or log out and back in.

## Matching CoreDisplay at runtime

CoreDisplay's size check has two recognized instruction layouts: one branches
after each comparison, while another combines comparison results with
`SETcc` instructions. DuoLink reads their stack displacements from the running
code so its replacement uses the same local variables. Bazel assembles both
replacements from source, allowing one release build to work without the
target installation's dyld cache.
[Captured instructions](../tests/fixtures/core_display.json) ·
[Replacement templates](../src/display/display_mode.s.in)

Only stack displacements may vary within a match; opcodes, registers,
constants, and relative branches must retain their expected encodings.
Repeated loads must name the same local, and the accessed locals must occupy
separate storage below `RBP`. These checks allow different compiler frame
layouts without assuming a fixed frame size or alignment. The selected
pattern and operands are carried into the patch operation, which rechecks
the matched bytes before writing.
[Matcher and patch operation](../src/display/display_patch.hpp)

| Stack value | Requirement preserved by the replacement |
| --- | --- |
| Width and height | Ordinary desktop modes satisfy 800×600; the exception requires native 1920×515. |
| Format check | The branch form compares a pixel-format code with four; the SETcc form consumes an eligibility byte computed earlier. |
| Mode pointer | Driver flags are read from the original mode record. |
| Saved EDI | Restore EDI after using it as scratch for width comparisons. |

The television and native exceptions use IOGraphics mode flags. A mode
without the driver's valid flag still follows Apple's safe mirroring/AirPlay
classification. The assembly names those flag bits and explains the output
masks, while the captured replacements establish the exact bytes that must
be preserved.
[IOGraphics flag definitions](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOGraphicsTypes.h) ·
[Branch-form replacement](../src/display/display_patch.s.in) ·
[SETcc-form replacement](../src/display/display_mode.s.in)

Reordering the block changes addresses inside it, so DuoLink rejects direct
branches from elsewhere on the same page into its interior. The guard uses
Capstone's full x86 instruction decoder from the pinned Lilu source to
distinguish branch instructions from opcode-like bytes inside operands.
Decoding uses local state and constant tables, with no allocation or locks.
Ambiguous matches, conflicting instruction boundaries, or a prefix that
cannot be decoded through to the mode check leave the page unchanged.
[Branch validation](../src/display/control_flow.hpp) ·
[Decoder adapter](../src/display/decode_instruction.c)

The guard examines branch encodings contained within the validated page. It
does not inspect other pages or resolve indirect jump targets. Tests include
the captured CoreDisplay page, SIMD instructions, and incoming jumps and
calls; they verify both rejection and preservation of the original bytes.
[Control-flow regression tests](../tests/compatibility/check_display.cpp)

`DisplayStatus` reports `waiting-for-CoreDisplay-page` until DuoLink finds a
recognized layout, then `patch-applied` after applying or finding the
replacement. A matching block that fails revalidation or the branch guard
reports `unsupported-CoreDisplay-control-flow`. Other instruction layouts
remain unchanged.
[Page validation hook](../src/display/kern_display.cpp)
