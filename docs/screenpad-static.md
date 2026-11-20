# ScreenPad Plus static after graphics initialization

On the UX481FL, OpenCore's boot picker and the Apple logo appear on the primary display while ScreenPad Plus stays black with its backlight on. On affected boots, the secondary panel began showing static when `AppleIntelCFLGraphicsFramebuffer` brought it online. The GPU switched to an alternate DisplayPort scrambler seed while the panel kept its default, so the receiver failed to decode the video after link training.

## Scrambler seed mismatch on Ventura 13.1

The BOE087F panel's EDID lists 1920×515 at 60.075 Hz with a 70.60 MHz pixel clock as its first detailed timing. On an affected Ventura 13.1 boot, the framebuffer driver selected that timing and logged `fPortType=0x2`, `ASR: 1`, and `Link Training successful` for framebuffer 1. [EDID posted by Qonfused in the ASUS ZenBook Duo Hackintosh repository][screenpad-edid] · [Framebuffer log](screenpad-captures.md#framebuffer-log)

[screenpad-edid]: https://github.com/Qonfused/ASUS-ZenBook-Duo-14-UX481-Hackintosh/pull/2#issuecomment-1212726281

While the panel showed static, its DisplayPort Configuration Data (DPCD) showed that both lanes had passed training and that the receiver still used the default seed. Intel's driver sent the first training pattern with scrambling disabled, so clock recovery could succeed even with different seeds at each end of the link. The receiver needed the same seed as the GPU once scrambled video followed. [Training-pattern write](screenpad-captures.md#first-training-pattern) · [Receiver register dump](screenpad-captures.md#receiver-registers)

| DPCD address | Value | Interpretation |
| --- | --- | --- |
| `0x202` | `0x77` | Both lanes had clock recovery, channel equalization, and symbol lock. |
| `0x00d` | `0x0b` | Bit 0 advertises support for Alternate Scrambler Seed Reset (ASSR). |
| `0x10a` | `0x00` | Bit 0 was clear, leaving the receiver on its default seed. |

Linux's DisplayPort header defines the masks for these values. [Lane status bits](https://github.com/torvalds/linux/blob/v6.12/include/drm/display/drm_dp.h#L775-L783) · [ASSR capability bit](https://github.com/torvalds/linux/blob/v6.12/include/drm/display/drm_dp.h#L166-L167) · [ASSR enable bit](https://github.com/torvalds/linux/blob/v6.12/include/drm/display/drm_dp.h#L636-L637)

On Ventura 13.1 (22C65), `AppleIntelFramebufferController::LinkTraining` writes the receiver's ASSR setting only for port type `0x3` (embedded DisplayPort). The ScreenPad's type, `0x2`, takes the branch at `0x1477aa06` past the write to DPCD `0x10a`. Both paths then program the GPU's DDI B control register. On the affected boot, the skipped write left BOE087F on its default seed while the GPU used the alternate seed. [Receiver branch and AUX call](screenpad-captures.md#receiver-write) · [Transmitter register write](screenpad-captures.md#transmitter-register)

We tested the effect of that missing write by changing only bit 0 of BOE087F's configuration over the DisplayPort auxiliary channel (AUX). The desktop became readable when `0x10a` read back as `0x01`. Restoring the original byte (`0x00`) brought the static back at the same 1920×515 timing. [AUX write, readback, and observations](screenpad-captures.md#aux-trial)

## Enabling ASSR during link training

BOE087F can lose its ASSR setting when power cycles, so DuoLink checks it when link training starts. It resolves the driver's `WriteAUX` symbol and looks for the successful write of `0x21` to DPCD `0x102` on framebuffer 1. This marks the start of training both at boot and when ScreenPad Plus comes back online. DuoLink then reads the DDI B control register to confirm that the GPU's transmitter is active and using ASSR. [Symbol lookup and framebuffer checks](../src/screenpad/kern_assr.cpp) · [Training-pattern check](../src/screenpad/assr.hpp#L15-L21)

If the receiver supports ASSR and still has it disabled, DuoLink checks the EDID vendor, product, and checksum before changing the configuration byte. It sets only bit 0 and reads back the entire byte to verify the write. If the panel already uses ASSR, the check finishes without a write. [Receiver checks and readback](../src/screenpad/assr.hpp#L33-L66) · [Panel configuration test](../tests/assr_test.cpp)

## ASSR status in IORegistry

Run `ioreg -l -r -c DuoLink` after boot. To check retraining, compare `ASSRFB1TrainingCalls` before and after turning ScreenPad Plus off and back on. On macOS 13.7.8 (22H730), one boot recorded four training starts and one verified receiver write. Its final status was `ASSR-already-enabled`, meaning the last check found the bit set. [IORegistry capture](screenpad-captures.md#duolink-boot-on-ventura-1378)

| Status | Meaning |
| --- | --- |
| `ASSR-enabled` | DuoLink enabled ASSR and verified the write. |
| `ASSR-already-enabled` | ASSR was already enabled at the latest check. |
| `ASSR-source-disabled` | DDI B did not report an active transmitter using ASSR, so DuoLink skipped the panel write. |

Use `-duolinkassroff` to disable only the ASSR fix.
