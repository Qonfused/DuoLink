# UX481FL ScreenPad captures

Framebuffer messages, register reads, and disassembly below are from macOS 13.1 (22C65) on the UX481FL. The final IORegistry excerpt is from a separate boot on macOS 13.7.8 (22H730).

## Framebuffer log

These messages from `framebuffer.log` show the mode selection and link training for framebuffer 1 on a boot where ScreenPad Plus showed static.

```text
2026-11-16 07:32:51.789089-0600  localhost kernel[0]: (AppleIntelCFLGraphicsFramebuffer) [IGFB][LOG  ][DISPLAY   ] [Modeset] FB1: Setting display mode 1920 x 515 -> 0 x 0 at 70600000 Hz encoded with 0x1  2 bpc with color 1 and range 1
2026-11-16 07:32:51.795971-0600  localhost kernel[0]: (AppleIntelCFLGraphicsFramebuffer) [IGFB][LOG  ][LINK_TRAINING] [Link_Training] fPortType=0x2, Lanes=2, BitRate=0xa, EnhancedFraming=1, ASR: 1, downspread:1 (forceIdle:0 :CR:100 EQ:400)
2026-11-16 07:32:51.798889-0600  localhost kernel[0]: (AppleIntelCFLGraphicsFramebuffer) [IGFB][LOG  ][LINK_TRAINING] [Link_Training] Clock Recovery complete
2026-11-16 07:32:51.800221-0600  localhost kernel[0]: (AppleIntelCFLGraphicsFramebuffer) [IGFB][LOG  ][LINK_TRAINING] Link Training successful
```

## Receiver registers

AGDCDiagnose lists ScreenPad Plus as port 2 in `agdcdiagnose.log`. Its output labels the ASSR bit as “capable” in both places. Register `0x00d` advertises support and register `0x10a` records whether ASSR is enabled. Ellipses mark gaps between the captured lines.

```text
## Register Dump Port 2 - Start ##
...
  Reg: 00000d: 0b : eDP_CONFIGURATION_CAP: ALTERNATE_SCRAMBLER_RESET_CAPABLE: 1, FRAMING_CHANGE_CAPABLE: 1
...
  Reg: 00010a: 00 : eDP_CONFIGURATION_CAP_SET: ALTERNATE_SCRAMBLER_RESET_CAPABLE: 0, FRAMING_CHANGE_CAPABLE: 0
...
  Reg: 000202: 77 : LANE0: CR_DONE: 1, CHANNEL_EQ_DONE: 1, SYMBOL_LOCKED: 1
  Reg: 000202: 77 : LANE1: CR_DONE: 1, CHANNEL_EQ_DONE: 1, SYMBOL_LOCKED: 1
...
## Register Dump Port 2 - End ##
```

## LinkTraining disassembly

These addresses are from `AppleIntelFramebufferController::LinkTraining` in Intel framebuffer version 20.0.2, image UUID `E88BAA5A-34CD-3A13-8504-ACFCB4DCD9B2`. Each excerpt preserves the addresses and operands from `LinkTraining.asm`. Ellipses mark omitted instructions.

### Receiver write

When port type differs from 3, the branch at `0x1477aa06` skips the one-byte AUX write to DPCD `0x10a`. On the fall-through path, the comparison at `0x1477aa41` supplies the Boolean value written to the receiver.

```asm
000000001477aa01	cmpl	$0x3, 0x28(%r13)
000000001477aa06	jne	0x1477aaab
...
000000001477aa41	cmpb	$0x0, -0x50(%rbp)
000000001477aa45	leaq	-0x62(%rbp), %r8
000000001477aa49	setne	(%r8)
...
000000001477aa5f	movl	$0x10a, %edx                    ## imm = 0x10A
000000001477aa64	movl	$0x1, %ecx
000000001477aa69	movq	%r13, %r9
000000001477aa6c	callq	__ZN31AppleIntelFramebufferController8WriteAUXEP21AppleIntelFramebufferjtPvP21AppleIntelDisplayPath ## AppleIntelFramebufferController::WriteAUX(AppleIntelFramebuffer*, unsigned int, unsigned short, void*, AppleIntelDisplayPath*)
...
000000001477aaab	movq	-0x38(%rbp), %rsi
```

### Transmitter register

Both paths read the ASSR selection from `-0x50(%rbp)` and place it in bit 6 of the transmitter control register. The same write sets the transmitter enable bit (31). For DDI B, the indexed register address resolves to `0x64140`.

```asm
000000001477ab4e	cmpb	$0x0, -0x50(%rbp)
000000001477ab52	setne	%cl
...
000000001477ab6b	shll	$0x6, %ecx
000000001477ab6e	orl	%eax, %ecx
000000001477ab70	movq	%rcx, -0xa8(%rbp)
000000001477ab77	movl	%ecx, %edx
000000001477ab79	orl	$0x80000000, %edx               ## imm = 0x80000000
000000001477ab7f	movzbl	0x450(%r13), %esi
000000001477ab87	shlq	$0x8, %rsi
000000001477ab8b	addq	$0x64040, %rsi                  ## imm = 0x64040
000000001477ab92	movq	-0x48(%rbp), %rbx
000000001477ab96	movq	%rbx, %rdi
000000001477ab99	callq	__ZN31AppleIntelFramebufferController15WriteRegister32Emj ## AppleIntelFramebufferController::WriteRegister32(unsigned long, unsigned int)
```

### First training pattern

After configuring the transmitter, the driver sends `0x21` to DPCD `0x102`. DuoLink recognizes this AUX transaction as the start of training.

```asm
000000001477abbb	leaq	-0x29(%rbp), %r8
000000001477abbf	movb	$0x21, (%r8)
000000001477abc3	movq	%rbx, %rdi
000000001477abc6	movq	%r12, %rsi
000000001477abc9	movl	$0x102, %edx                    ## imm = 0x102
000000001477abce	movl	$0x1, %ecx
000000001477abd3	movq	%r13, %r9
000000001477abd6	callq	__ZN31AppleIntelFramebufferController8WriteAUXEP21AppleIntelFramebufferjtPvP21AppleIntelDisplayPath ## AppleIntelFramebufferController::WriteAUX(AppleIntelFramebuffer*, unsigned int, unsigned short, void*, AppleIntelDisplayPath*)
```

## AUX trial

Complete output from `runtime-trial-1.log`. The tool opened framebuffer 1, verified BOE087F, and read back its configuration byte after each write.

```text
Verified FB1, BOE087F; native DisplayPort interface opened.
DPCD 0x00000: 11 0a 02 41 00 00 01 00 02 00 00 00 00 0b 00 00
DPCD 0x00100: 0a 82 00 01 01 01 01 10 00 00 00
DPCD 0x00200: 01 00 77 00 01 01 11 00
TRIAL: FB1/BOE087F only; DPCD 0x10a 00 -> 01; restore after 15 s.
Trial write: 0x00000000; read: 0x00000000; value: 01
RESTORE: write=0x00000000 read=0x00000000 value=00 expected=00
```

The desktop became readable after the tool read back `01`. Static returned when the tool restored and read back `00`.

## DuoLink boot on Ventura 13.7.8

Selected properties from DuoLink 0.2.0 in `pen-baseline.txt` on build
22H730. The verified-write count covers the whole boot, while `Status`
records the most recent check.

```text
"ASSRFB1TrainingCalls" = 4
"VersionInfo" = "0.2.0"
"Status" = "ASSR-already-enabled"
"ASSRVerifiedWrites" = 1
"ASSRLastAuxError" = 0
```
