# Touch and pen input on the wrong display

On the UX481FL, tapping the center of ScreenPad Plus could put the cursor at the
center of the main display. The same tap landed correctly after moving the
cursor onto ScreenPad Plus with the trackpad. Each digitizer supplied
coordinates relative to its panel, while macOS applied them to the display under
the cursor. [VoodooI2C issue
#96](https://github.com/VoodooI2C/VoodooI2C/issues/96) describes the same
behavior on a Dell laptop with an external display.

## Matching input to its panel

DuoLinkTouch matches each digitizer’s HID identity to its panel’s EDID identity.
It converts digitizer coordinates using that framebuffer’s desktop bounds, so
mapping follows the panel when you rearrange Displays. The UX481FL identities
are collected in [devices.hpp](../src/input/devices.hpp), while
[display_mapping.hpp](../src/touch/display_mapping.hpp) defines the
coordinate conversion.

| Display | Digitizer (ACPI) | HID vendor/product | Panel | EDID vendor/product |
| --- | --- | --- | --- | --- |
| Main | ELAN9008 | `04f3:2b6a` | CMN14D5 | `0dae:14d5` |
| ScreenPad | ELAN9009 | `04f3:29de` | BOE087F | `09e5:087f` |

With ScreenPad Plus directly below a 1080-pixel main display, its desktop rows
run from 1080 through 1594. DuoLinkTouch positions the cursor within those
bounds before forwarding native digitizer dispatch, allowing the first tap or
pen hover to select ScreenPad Plus. Finger clicks, drags, scrolling, and pen
events continue through the original VoodooI2CHID methods. [Mapping
implementation](../src/touch/touchscreen.cpp)

VoodooI2CHID carries cached pen coordinates in finger reports. To avoid
mistaking those reports for new pen input, DuoLinkTouch requires both
coordinate timestamps to match the current report before selecting pen
handling. It requests another absolute position when a gesture ends, a pen
reenters range, the framebuffer changes, or its desktop bounds move. Native
finger-release events bypass repositioning so lifting a finger cannot move
the cursor back to old contact coordinates. [Native report
access](../src/input/native_hid.cpp) · [Input handlers](../src/touch/touchscreen.cpp)

## Checking input

After reboot, run:

```sh
ioreg -l -r -c DuoLink
ioreg -l -r -c DuoLinkTouch
ioreg -l -r -c VoodooI2CTouchscreenHIDEventDriver
```

Check the following properties to confirm that both driver hooks and panel
mapping are ready:

| Service | Property | Expected value |
| --- | --- | --- |
| DuoLink | `InputCoreStatus` | `"stock-core-ready"` |
| DuoLink | `TouchRouteStatus` | `"stock-HID-ready"` |
| DuoLink | `TouchRoutesReady` | `Yes` |
| DuoLink | `TouchCallbacksRegistered` | `Yes` |
| DuoLinkTouch | `NotificationStatus` | `"registered"` |
| Each native touchscreen | `TouchMappingRole` | `"main"` or `"ScreenPad"` |

Start with the cursor on the main display and hover over ScreenPad Plus. The
cursor should appear under the pen before it touches the glass. Lift the pen out
of range and repeat on the main display, then test finger taps in both
directions. Also check rapid taps and swipes, touchpad dragging with its
physical button held, and input after sleep and wake.

`TouchMappingStatus = "positioned"` records successful cursor positioning.
Report counters, reposition counts, and last coordinates are published on the
first report and every 64 reports thereafter; they are sampled diagnostics
rather than a record of every event. For packet capture or touchpad
initialization checks, see [Input driver compatibility](input-drivers.md).

Use `-duolinktouchoff` to disable HID routing, including display mapping and touchpad feature initialization. The core GPIO correction remains active. To skip all DuoLink hooks, use `-duolinkoff`. DuoLink reports `Status = "disabled"` in IORegistry.
