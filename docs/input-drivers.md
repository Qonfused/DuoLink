# Input driver compatibility

DuoLink uses Lilu to hook the official VoodooI2C and VoodooI2CHID 2.9.1 release
in memory. DuoLinkTouch supplies display mapping through callbacks registered
with DuoLink, allowing the companion to use native input handling without
linking against either driver's classes. Install both drivers from the
official archive. [Route installation](../src/input/kern_input.cpp) ·
[Callback interface](../src/input/routing_api.hpp)

## Recognizing a supported binary

[MODULE.bazel](../MODULE.bazel) pins the release archive by SHA-256. During
the build, [generate_hid_profile.py](../tools/generate_hid_profile.py) verifies
the HID binary against its debug symbols. It uses type names to find fields
such as `ValueWithTime.timestamp`, then checks field bounds, virtual method
slots, and native call sites before writing the C++ profile used by DuoLink.

At boot, DuoLink checks each driver's UUID and method entry bytes against that
profile before installing hooks. HID hooks also require the core routes and
a matching touchscreen virtual table. These checks catch changes that a
version string alone cannot identify and report a failed check in IORegistry.
[Runtime checks](../src/input/kern_input.cpp)

## GPIO interrupts

During creation of the touchscreen's interrupt event source, macOS queries
the trigger type before VoodooGPIO records it during handler registration.
An early edge-triggered result leaves a level-triggered line unmasked while
the driver reads a report, allowing repeated callbacks to queue redundant
reads and stall input. For `ELAN9008` and `ELAN9009`, DuoLink reports
the trigger type declared in the ACPI flags in `gpioIRQ`, allowing macOS to
mask the line until the read finishes. `GPIOTypeCorrections` counts corrected
results in IORegistry. [Interrupt-type hook](../src/input/kern_input.cpp)

DuoLink also finds VoodooGPIO through IOKit's service registry and checks its
type with `metaCast`. This avoids the static cross-extension metaclass
reference described in [VoodooI2C PR #589](https://github.com/VoodooI2C/VoodooI2C/pull/589),
where an invalid relocation can cause a page fault during controller lookup.
[Installation](installation.md)

## Native HID state

DuoLink clears VoodooI2CHID's cached framebuffer when it becomes inactive or
differs from the panel selected by DuoLinkTouch. The native cache field must
be cleared before its reference is released because release can trigger
callbacks that read the field again. DuoLinkTouch retains its selected
framebuffer independently. [Cache invalidation](../src/input/native_hid.hpp)

VoodooI2CHID normalizes pen Z and pressure samples by their declared ranges.
If either range is zero or negative, DuoLinkTouch supplies a sample of zero
and a range of one during the native call, then restores both fields. This
lets the call handle an absent or invalid range without dividing by zero or
producing a value from an invalid scale. [Range guard](../src/input/native_hid.hpp)

## Precision Touchpad features

Surface Switch and Button Switch control touch-surface and button reporting
in Precision Touchpad mode. VoodooI2CHID 2.9.1 selects that mode without
enabling those switches; [VoodooI2CHID PR #82](https://github.com/VoodooI2C/VoodooI2CHID/pull/82)
adds the requests for devices that start with reporting disabled.
DuoLinkTouch issues those requests for the UX481FL's ELAN1207 (`04f3:310e`)
after native mode selection, including after wake. It first verifies both
one-bit fields in Feature report 5 under Device Configuration → Finger.
[Touchpad initialization](../src/touch/touchpad.cpp)

Run `ioreg -l -r -c VoodooI2CPrecisionTouchpadHIDEventDriver` and look for
`DuoLinkPTPStatus = "enable-requested"` and `DuoLinkPTPFeatureFields = 2`.
These values record that `setDataValue` was called for both fields. Because
that method returns no status, they cannot confirm that the device accepted
either request.

## Raw input diagnostics

Add `-duolinkpenwire` and reboot to count I²C input frames before HID report
validation. Read `PenWireMain` and `PenWireScreenPad` in IORegistry. Packet
counts accumulate as reports arrive, while IORegistry refresh requests are
limited to four per second. Remove the boot argument to disable the probe.
[I²C probe](../src/diagnostics/kern_pen_wire.cpp) ·
[Touchscreen counters](input-mapping.md#checking-input)
