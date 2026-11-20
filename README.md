# DuoLink

DuoLink is a Lilu plugin for the ASUS ZenBook Duo UX481FL. It handles fixing ScreenPad
Plus display initialization, makes the panel available in System Settings, and keeps the Dock on
the main display when ScreenPad Plus sits below it.

Its bundled companion kext, DuoLinkTouch, maps finger and pen input to the display
you touch while preserving native input handling in
[VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) and
[VoodooI2CHID](https://github.com/VoodooI2C/VoodooI2CHID). Both drivers can be
installed from their official release.

## macOS Compatibility

DuoLink supports macOS Catalina through Tahoe on the UX481FL. The default test
suite checks ScreenPad link-training hooks and Dock placement against these
macOS builds:

| macOS | Version | Build |
| --- | --- | --- |
| Catalina | 10.15.7 | `19H2` |
| Big Sur | 11.6 | `20G165` |
| Monterey | 12.5.1 | `21G83` |
| Ventura | 13.4.1 | `22F82` |
| Sonoma | 14.6.1 | `23G93` |
| Sequoia | 15.4.1 | `24E263` |
| Tahoe | 26.6.2 | `25G83` |

The link-training tests resolve ASSR driver symbols and check that the
`WriteAUX` method's opening instructions can be routed. Dock tests check
placement against eight executables, including Ventura 13.1 (`22C65`) in
addition to the builds above. CoreDisplay tests reproduce captured
replacements and check that incoming branches on the matched page prevent
patching. [Binary compatibility tests](docs/development.md#binary-compatibility-tests)

## Build

Build on an Intel or Apple silicon Mac with Xcode or Command Line Tools:

```sh
./bazelw build //:DuoLink
./bazelw test //tests:all
```

Kernel extensions are built for Intel, while test programs compile for the
build Mac's CPU. Note that display and Dock fixes identify their target instructions at
runtime, so building does not require the macOS installation you plan to boot.
[Build instructions](docs/development.md#build-and-test)

Build distributable ZIPs with the [release configurations](docs/releases.md).

## Install with OpenCore

Install `bazel-bin/DuoLink.kext`, which includes DuoLinkTouch. Use Lilu 1.6.5 or
later and the VoodooI2C and VoodooI2CHID bundles from the official 2.9.1 release.
Add a separate OpenCore entry for the VoodooGPIO build listed in the
installation guide. [Installation and load order](docs/installation.md)

## Verify after reboot

| Feature | Check |
| --- | --- |
| [ScreenPad link training](docs/screenpad-static.md) | ScreenPad Plus stays black during the boot picker and Apple logo. Check for a clear image when Intel graphics brings it online. |
| [Displays and Dock](docs/displays-and-dock.md) | Place ScreenPad Plus directly below your main display in Displays. Set Dock to Bottom and apply the linked display-switching preference, then check that it stays on the upper panel. |
| [Touch and pen mapping](docs/input-mapping.md) | With the pointer on one display, tap the other with a finger or hover over it with a pen. Repeat in the opposite direction. |

Use `-duolinkoff` to skip installing DuoLink's hooks, or `-duolinkdbg` with a
DEBUG build for additional Lilu startup logging.

## License

DuoLink is licensed under the [BSD 3-Clause license](LICENSE).
