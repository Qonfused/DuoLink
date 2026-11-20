# Installation

Back up your working EFI configuration before installing DuoLink. Use Lilu
1.6.5 or later and a [local build](development.md#build-and-test) or release
archive containing DuoLink and its bundled DuoLinkTouch companion.

## Input drivers

Use `VoodooI2C.kext` and `VoodooI2CHID.kext` from the official VoodooI2C 2.9.1
RELEASE archive. DuoLink checks their binary UUIDs and method entries before
installing hooks. [VoodooI2C 2.9.1](https://github.com/VoodooI2C/VoodooI2C/releases/tag/v2.9.1) ·
[Driver compatibility](input-drivers.md)

Build upstream VoodooGPIO at commit `44fc744` and copy the resulting bundle to
`EFI/OC/Kexts/VoodooGPIO.kext`. Disable the OpenCore entry for the older
`VoodooI2C.kext/Contents/PlugIns/VoodooGPIO.kext`, leaving that bundled copy in
place so OpenCore injects only the standalone build.
[VoodooGPIO revision](https://github.com/VoodooI2C/VoodooGPIO/commit/44fc744d26b3e4971d6d8f0027d75c25e1388637)

## Touchscreen interrupts

Both touchscreens must use GPIO interrupts for DuoLink's interrupt-type
correction. The [published UX481 SSDT](https://github.com/Qonfused/ASUS-ZenBook-Duo-14-UX481-Hackintosh/blob/44d087bff6c75688a0a5d7882e421af8c7bb8230/src/ACPI/SSDT-GPIO.dsl)
uses APIC resources for those devices and includes commented GPIO resources
named `SBFG`. In each touchscreen scope, uncomment its `SBFG` resource and
replace the `SBFA` return in the Darwin `_CRS` branch with:

```asl
Return (ConcatenateResTemplate (SBFB, SBFG))
```

Keep the `GpioInt` descriptors set to `Level, ActiveLow` with these pins:

| Scope | GPIO pin |
| --- | --- |
| `_SB.PCI0.I2C0.TPL1` (main touchscreen) | `0x72` |
| `_SB.PCI0.I2C3.TPL0` (ScreenPad Plus) | `0x32` |

Compile the table with `iasl SSDT-GPIO.dsl` and replace
`EFI/OC/ACPI/SSDT-GPIO.aml`. Keep the existing OpenCore `_CRS` to `XCRS`
renames for both touchscreens. If you used `-vi2c-use-acpi-irq-type` with an
earlier VoodooI2C fork, remove it from boot arguments; DuoLink provides that
correction for the two touchscreens with stock drivers.

## OpenCore entries

Copy Lilu, DuoLink, and both official input bundles into `EFI/OC/Kexts`
alongside the standalone VoodooGPIO. Enable the entries below in
`Kernel → Add` in the order shown. Set `PlistPath` to `Contents/Info.plist`,
leave `MinKernel` and `MaxKernel` empty, and set `Arch` to `x86_64` for both
DuoLink extensions.

| BundlePath | ExecutablePath |
| --- | --- |
| `Lilu.kext` | `Contents/MacOS/Lilu` |
| `DuoLink.kext` | `Contents/MacOS/DuoLink` |
| `VoodooI2C.kext/Contents/PlugIns/VoodooI2CServices.kext` | `Contents/MacOS/VoodooI2CServices` |
| `VoodooGPIO.kext` | `Contents/MacOS/VoodooGPIO` |
| `VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext` | `Contents/MacOS/VoodooInput` |
| `VoodooI2C.kext` | `Contents/MacOS/VoodooI2C` |
| `VoodooI2CHID.kext` | `Contents/MacOS/VoodooI2CHID` |
| `DuoLink.kext/Contents/PlugIns/DuoLinkTouch.kext` | `Contents/MacOS/DuoLinkTouch` |

Disable any earlier standalone ASSR entry in `Kernel → Patch`. Run
`ocvalidate` from your OpenCore release, then reboot to load the extensions.
Follow the [display setup](displays-and-dock.md) to arrange both panels and
keep Dock on the main display.
