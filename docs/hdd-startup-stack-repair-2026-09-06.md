# HDD startup stack and Doom compatibility repair — 2026-09-06

Status: the emulated qualification below completed and the CD was written
(xorriso exit 0, track closed). The user subsequently confirmed that the
physical HDD installation still returns corrupt directory names, a beep, and
`cannot execute` / `EXEC FAIL 0002`. The reported physical regression remains
unresolved. These emulator results do not establish a fix for that failure.
See [the UI/EXEC isolation investigation](ui-exec-isolation-2026-09-06.md).

The reported physical-machine symptom is an indefinite blinking cursor after
the splash, without startup audio, on the newly installed T23 HDD. F4 and
Enter have no effect after one minute. This report records a reproduced
startup-memory defect; the repaired build still needs a retry on that T23.

## Defect and change

BOOTSND.COM reserved only 1024 bytes for its entire stack. PCI BIOS 2.1,
section 3.2, requires 1024 bytes available for the BIOS itself. CiukiOS's DOS
dispatcher and disk calls also use the application's stack, adding their
frames while BOOTSND loads its PCM file. The caller therefore needs more
than a 1024-byte total allocation.

Reference: [PCI-SIG PCI BIOS Specification 2.1, section 3.2, page 3](https://www-online.kek.jp/~yasu/Parallel-CAMAC/bios21spec.pdf#page=7).

The product change increases the player's reserved stack to 4096 bytes in
`src/com/boot_sound.inc`. The same binary supplies SYSTEM/BOOTSND.COM and
SYSTEM/DRIVERS/SOUND.COM. The extra 3072 bytes are transient and are released
when the player exits. Kernel, native shell and Windows files remain
byte-identical to the preceding release. The subsequent Doom investigation
below is independent of this startup change.

## Reproduction

`scripts/fixtures/firmware_stack.asm` is a test-only option ROM. It reserves
1 KiB of firmware RAM, installs INT 13h/INT 1Ah wrappers and consumes a
1024-byte scratch stack footprint before delegating to the real SeaBIOS
function with its original registers and stack. Six bytes of the interrupt
frame and twelve bytes of saved state are included in that footprint. The
scratch is filled with A55Ah to identify writes. The ROM does not manufacture
DOS return values or replace an application and is absent from the CD.

The disk-firmware footprint is an explicit test model; it is not a measured
T23 BIOS trace. PCI's documented stack allowance provides the other case.
`scripts/qemu_test_startup_stack.py` boots a disposable copy of the installed
HDD and records actual fixture call counts, PCM, screen images, registers
and the first MiB of RAM.

With the shipped 1024-byte player, the combined model prevents the desktop
from becoming ready. Captured memory shows the player's saved master/slave
PIC masks changed to A5h/5Ah and its PCM buffer segment changed to A55Ah.
These are the firmware scratch bytes, beyond the player's allocated stack.
The kernel's checked 38661 code bytes remain intact, locating this failure
in player state. The emulated failure includes corrupted audio and garbled
text; its display differs from the reported physical blinking cursor.

Changing only the player's stack to 4096 bytes makes the identical model
reach the desktop, emit normal startup PCM, execute DOS ECHO and COMDEMO,
and return to the desktop. The fixture records 838 disk and eight PCI calls.
PCM RMS is 3546 with peak 14646; all checked kernel code remains intact.
The ordinary BIOS path passes the same desktop/DOS/audio sequence.

Separate controls isolate the transfer path: the old player still fails with
only the disk wrapper enabled, while the PCI-only wrapper reaches the desktop
with normal audio. Thus the observed overwrite is specifically the nested
DOS/disk stack use; this is not evidence that PCI discovery alone caused the
reported failure. Both wrappers together pass with the larger player stack.

Evidence:

- [Before: failed startup](../build/full/hdd-startup-repair-2026-09-06/before-both/result.json).
- [Corrupted player state and kernel comparison](../build/full/hdd-startup-repair-2026-09-06/memory-comparison.json).
- [After: firmware stack case](../build/full/hdd-startup-repair-2026-09-06/after-both/result.json).
- [After: ordinary BIOS](../build/full/hdd-startup-repair-2026-09-06/after-standard/result.json).
- [Old player: disk-only failure](../build/full/hdd-startup-repair-2026-09-06/before-disk/result.json).
- [Old player: PCI-only control](../build/full/hdd-startup-repair-2026-09-06/before-pci/result.json).

## Rebuilt ISO and installed HDD

The actual GRUB Setup entry installed the rebuilt ISO onto a disposable HDD.
Every installed sector was compared with the source; only the expected GRUB
identity and D-to-C boot patch differ. The HDD then booted without a CD and
accepted shell commands. Its SHA-256 is
`18c460d942832e87147ec73e45697a5ea3fe025333e37c0c4c11431082425fcc`.
[Installation log](../build/full/hdd-startup-repair-2026-09-06/install.log).

The combined firmware test also passes on a copy of this new installation,
with no sound, shell or kernel replacement: 838 real disk calls, eight PCI
calls, desktop/DOS return and startup PCM RMS 3546, peak 14646.
[Installed firmware result](../build/full/hdd-startup-repair-2026-09-06/final-firmware/result.json).

Windows 3.1 passes two sessions, Calculator 2+3=5, resize and close with zero
residual changed pixels, WAV/MIDI playback and DOS return on the same new
HDD. PCM RMS is 1184 for WAV and 706 for MIDI.
[Windows result](../build/full/hdd-startup-repair-2026-09-06/windows-800/result.json).

## Classic Doom regression found during acceptance

The first seven HDD cases passed. The final classic-Doom case failed while
still showing its main menu. The captured master PIC has IRQ0 in service
(`isr=01`, `irr=07`), leaving keyboard IRQ1 pending. This occurred with the
preceding launcher and its existing `/CF4` option. The failure is retained in
[the first application matrix](../build/full/hdd-startup-repair-2026-09-06/installed-acceptance/classic-doom/results.json).
It prevents treating the earlier cold-start successes as proof of a universal
fix for this intermittent problem.

The pinned VSBHDA author's [SETPVI/RESPVI documentation, section 4.2](https://github.com/Baron-von-Riedesel/VSBHDA/blob/main/vsbhda.txt)
recommends trying CR4.PVI for protected-mode games that freeze, including
Rational DOS/4GW applications. `src/com/doom_launch.asm` applies this setting
only around original Doom, checks CPUID/VME support and real mode first, and
restores the caller's PVI bit after unloading the transient host or on failure.
It preserves the other CR4 bits and leaves other game launchers and drivers
unchanged. This is a documented compatibility mitigation; it does not prove
that every possible Doom freeze has the same cause.

Two weaknesses in candidate qualification were corrected during this work:

- The shell's Doom shortcut uses `DOOM.EXE`. Replacing only `DOOM.COM` did not
  exercise the candidate. The test now accepts and hashes both formats and
  explicitly checks CR4 during gameplay and after exit. The rejected trial is
  retained in `doom-pvi-strict-1`: it reached a game but correctly failed the
  new assertion because PVI was absent. That trial is not a candidate success.
- Skull animation alone could satisfy the former menu-pixel-change assertion.
  The new `scripts/doom_screen.py` decodes the real WAD's two skull sprites and
  all gameplay palettes, including pickup flashes. It identifies the selected
  row, requiring main-menu row 0, Down to row 1, Up to row 0, episode row 0 and
  default skill row 2. Gameplay movement, audio, quit, COM execution, mouse and
  desktop/DOS return remain separate checks. The old frozen screenshots stay
  at row 0 after Down and fail the stronger criterion.

Four consecutive corrected-format cold starts pass the full interactive
sequence, audio, kernel-code comparison and DOS/desktop return. They record
CR4 `00000000` before Doom, `00000202` during gameplay and `00000200` after
exit: PVI is enabled and restored; the host's SSE bit is retained.
[Cold-start evidence](../build/full/hdd-startup-repair-2026-09-06/doom-pvi-cold-starts.json).

A separate test removes HDPMI only from a disposable final HDD to exercise
real EXEC failure. Both COM and MZ launchers restore the entire captured CR4
value, with PVI initially clear and initially set. COMDEMO and shell commands
work after each of the four failures.
[Failure-cleanup evidence](../build/full/hdd-startup-repair-2026-09-06/doom-cleanup-final/result.json).

## Final rebuilt installation

The second ISO includes the qualified Doom COM and MZ launchers. Setup again
copied every sector correctly and the target booted without a CD and accepted
DOS commands. Its SHA-256 is
`64f18a13b1c76e0071b8c963e21c2138549eb58dd4893b139ec987c5d2fc9410`.
[Final installation log](../build/full/hdd-startup-repair-2026-09-06/install-final.log).

Comparing all 1408 files against the first rebuilt installation finds exactly
two changed files: DOOM.COM and DOOM.EXE. Kernel, shell, sound player, Windows,
other applications and drivers are byte-identical, so the Windows 800×600
results above apply to those same payloads.
[Complete file comparison](../build/full/hdd-startup-repair-2026-09-06/installed-file-differences.json).

The final installed HDD also passes the combined firmware-stack model with
838 disk and eight PCI calls, normal startup PCM (RMS 3546, peak 14646), actual
DOS command/COM execution and desktop return. No product payload is replaced
in this final test.
[Final firmware result](../build/full/hdd-startup-repair-2026-09-06/firmware-final/result.json).

The complete eight-case final matrix passes on the same installed source and
records the hashes of both Doom formats, the boot/sound players, kernel and
shell. It checks 437 directory entries and fully reads/checksums 12 files
(total 25,649,394 bytes), including both Doom WADs. The forced BIOS fallback
performs real CHS reads and writes and compares the copied bytes. VGASETUP's
800×600 preview, cancel, confirmation and subsequent DOS input pass.

Wolf3D, Doom Vanille and classic Doom reach actual gameplay, respond to
movement, emit recorded PCM and quit to working DOS. DOS Navigator changes
panels and exits; Costa's pointer moves and it exits. The fifth corrected
classic-Doom cold start is from this final installation with no launcher
replacement. Its selected menu rows, CR4 lifecycle, subsequent COM program,
mouse movement and DOS/desktop return all pass. Across the eight cases, all
38661 checked kernel code bytes remain unchanged.
[Final matrix and payload hashes](../build/full/hdd-startup-repair-2026-09-06/installed-final/summary.json),
[final execution log](../build/full/hdd-startup-repair-2026-09-06/final-matrix.log).

| Final artifact | SHA-256 |
|---|---|
| ISO | `482aa94b50ce6037489474b2c0d5414ee75bcba886aae4fee43dddcc896abe98` |
| Raw CD disk | `9a482cbad6d6f4792a687985a36760391c3db8a43c8306e96c66fc91ee24fd8b` |
| DOOM.COM | `8567ac9d08ce77710cc954e27335dca86217e4a007b932e43fa40f009375632e` |
| DOOM.EXE | `941377fbb764f169baa765a474db4696c290ad9a3ef35e6a4c8b4f3a52682f56` |

## Initial artifact provenance

These hashes identify the first rebuilt ISO, before the Doom launcher trial.

| Artifact | SHA-256 |
|---|---|
| Previous BOOTSND.COM | `51bd55bcee4bb349e10e3cc2ce563be79100da54065f2048eaf847fbb740f3a9` |
| Corrected BOOTSND.COM and SOUND.COM | `728fe747c915d5249b14e56e222bfa363d00fee3e250c2d606dfc1b4eed0f8ab` |
| Unchanged CIUKIDOS.SYS | `7c482ad57a0bb44d3456fb0a5e999d13bbe5743468731a4e739f5186f671d1e3` |
| Unchanged SHELL.COM | `e5d54d2e25f67fa917cba0d22f66044eb0faf2cd394f84ca94d9ed5e32aaa700` |
| Startup-only ISO | `aa224f8ce317e98124233ffa7e45b69aa789d4f0ce246ceabacea135f7cd29cb` |
| Startup-only raw CD disk | `dd7686f668f94fdd7f75c54667b0edbe8ff8616c07b6635d513df9559f3594ec` |

The previous burned ISO is preserved as
`build/full/hdd-startup-repair-2026-09-06/before-CiukiOS.iso`, and its raw disk
as `before-cd-disk.img` beside it. Earlier application and installation
evidence is indexed in the [previous release report](hdd-release-2026-09-06.md).

The startup-only intermediate artifacts are preserved beside them as
`startup-only.iso` and `startup-only-cd-disk.img`.

## CD delivery

The final ISO was written to the inserted CD-RW and the track closed
successfully; xorriso returned 0. The command requested 4×; the drive's
reported sustained write rate was approximately 10×. No optical preflight
or readback verification was run. The burner performs its own required media
assessment and finalization.
[Burn log](../build/full/hdd-startup-repair-2026-09-06/burn.log),
[exit status and ISO/log hashes](../build/full/hdd-startup-repair-2026-09-06/burn-result.json).

This does not update the T23's already installed HDD. The repaired build must
be applied there before a physical-machine retry can establish whether its
post-splash symptom is resolved. The firmware model, QEMU AC97 audio and
application tests above do not substitute for that physical test.

The VS Code session remains capped at 8,000,000,000 bytes, with a
7,000,000,000-byte high watermark in `app-code-111849.scope`.
