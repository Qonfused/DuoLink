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

DuoLink changes the size decision in CoreDisplay's mode description. Its
matcher reads the width, height and pixel-format operands from the running
code and locates the driver-validity test immediately afterward. The write
ends at the start of that test, so CoreDisplay still decides how to classify
an invalid mode for mirroring or AirPlay. The length of that classification
can change independently of the size check.
[Matcher](../src/display/display_patch.hpp) ·
[Captured CoreDisplay pages](../tests/fixtures/core_display_pages/manifest.json)

Catalina calls XNU's range validator for a page fault and keeps its shared
cache under `/private/var/db/dyld`. DuoLink accepts a complete executable
page there once XNU validates it. Kernels exporting the page validator use
that callback directly. [Catalina VM page faults](https://github.com/apple-oss-distributions/xnu/blob/xnu-6153.141.1/osfmk/vm/vm_fault.c#L6485) ·
[Catalina dyld cache directory](https://github.com/apple-oss-distributions/dyld/blob/dyld-750.6/launch-cache/dyld_cache_format.h#L362)

Scalar layouts compare width and height separately. Some read a pixel format
code while others consume an eligibility bit computed earlier in the function.
The packed layout compares both dimensions against a vector in CoreDisplay.
DuoLink keeps that reference at its original instruction address so ordinary
modes use Apple's thresholds. Both paths admit native 1920×515 only after the
pixel format qualifies.
[Geometry and register assignments](../src/display/display_mode.s.in)

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
