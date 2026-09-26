# CiukiOS HDD release — 2026-09-06

This records the preceding CD delivery. A subsequent physical HDD startup
failure is tracked in the [startup-stack repair](hdd-startup-stack-repair-2026-09-06.md).
The ISO described here is preserved as `build/full/hdd-startup-repair-2026-09-06/before-CiukiOS.iso`;
the canonical ISO filename now contains the subsequent repair.

**Status: final ISO installation and application acceptance passed;
CD written successfully (xorriso exit 0).** The clock-redraw correction passed actual RTC minute changes
at 800×600 and automatic 1280×800, plus the complete native-window sequences.

The results below concern the archived release artifacts and disposable QEMU guests
with 512 MiB RAM. Physical ThinkPad T23 boot time, speakers, graphics and USB
controller behavior have not been measured in this qualification.

## Reproduced causes and corrections

- **BIOS disk fallback:** firmware register changes and a reset DAP sector count
  could redirect directory reads or writes. The disk wrappers preserve caller
  state across geometry/transfer calls and rebuild the transfer count for each
  operation. Fault-injected EDD failure followed by real CHS I/O now passes.
- **MZ PSP/MCB corruption:** EXEC published a new PSP before initializing its
  parent field. An allocator walk could interpret loaded program bytes as PSP
  ancestry and write an MCB into kernel instructions. Hardware watchpoints
  established the write. Initializing the minimal PSP before publishing it and
  bounding ancestry/MCB construction address this cause. See the
  [HDD investigation](installed-hdd-qualification-2026-09-06.md).
- **EXEC state:** suspended parent calls were counted in InDOS, and EXEC could
  retain the child's DOSMGR caller metadata. The correction balances actual
  child transfers and restores the parent frame. A before/after comparison
  covers 14 EXEC cases and observes InDOS=0 outside DOS, 1 during BIOS I/O;
  registers, errors, overlay contents and TSR restoration are checked.
  [EXEC report](dos-exec-state-repair-2026-09-06.md).
- **AH=40h/CX=0:** file sizes could grow beyond their allocated FAT chains.
  Extension/truncation now updates the chain and rolls back failed growth.
  Six complete read/copy cases pass with the current kernel; the independent
  full-volume rollback test is recorded in the
  [zero-write report](dos-zero-write-repair-2026-09-06.md).
- **Clock redraw:** `ui_clock_poll` called the compositor while ES still
  addressed the BIOS data area. The Run field's STOSB copied `echo retained`
  to physical A8BD, inside the kernel, rather than the shell's visible-text
  buffer. The resulting damage could remain invisible until the next DOS
  command. The clock now restores the shell segment before drawing, and the
  Run field independently saves and sets ES for its local string copies.
  The failed capture remains in `integrated-final-800`. A deterministic
  minute-change test reproduces 18 corrupted bytes with the old shell and
  none with the corrected shell, at both 800×600 and automatic 1280×800.
  [Native-window and clock report](native-windows-repair-2026-09-06.md).

## Evidence index

| Area | Verified behavior | Evidence |
|---|---|---|
| Native desktop | Multiple retained windows, focus, dragging, minimize/restore, cursor bounds, DOS return and actual RTC minute transitions pass at 800×600 and automatic 1280×800. All 38,661 checked kernel code bytes remain intact. | [Window and clock results](native-windows-repair-2026-09-06.md) |
| Startup | Original photograph matches captured frames; progress bar shows intermediate states. Startup-only AC97 recording contains 1.254 seconds of PCM, RMS 3574, peak 14646, before any application or input. | [Integrated 800 result](../build/full/ui-multiwindow-2026-09-06/integrated-fixed-800/result.json), [Startup sound](../build/full/ui-multiwindow-2026-09-06/integrated-fixed-800/startup-only.wav) |
| Final installed HDD | Eight cases, no payload replacement: 437 directory entries, twelve complete files / 25,649,394 bytes; all 38,661 main kernel code bytes retained. | [Acceptance summary](../build/full/hdd-release-final-2026-09-06/installed-acceptance/summary.json) |
| Applications | Original Doom, Doom Vanille and Wolf3D gameplay, movement, shooting, captured audio and return; DOS Navigator and Costa interaction; VGASETUP preview/cancel/save from DOS. | [Scope and per-case results](installed-hdd-qualification-2026-09-06.md) |
| DOS console | Automatic 1280×800 directory output and working next command in 0.974 seconds, including command entry. | [Video result](../build/full/hdd-release-final-2026-09-06/installed-acceptance/video/results.json) |
| File resizing | Six extension/truncation cases, complete guest copies and independent FAT-chain checks. | [Current-kernel results](../build/full/hdd-release-2026-09-06/zero-write/results.json) |
| Final ISO installation | Quick installation through the actual GRUB Setup entry; whole-image comparison, partition write bounds and HDD-only desktop/DOS boot pass. Installed payload hashes match the final artifacts. | [Final installation result](../build/full/hdd-release-final-2026-09-06/setup-install-result.json) |
| Setup | Real GRUB Setup entry; quick/full installations compared byte-for-byte with source, then booted from HDD without CD. Separate format-only tests distinguish preserved A5 data from full zeroing; writes stay inside the displayed partition. | [Setup results](../build/full/hdd-release-2026-09-06/setup-install-results.json) |
| External media | Existing installed MEDIA.COM; actual BIOS floppy00, USB81, CD E0 and legacy ATAPI170 backends observed in RAM. Four 8965-byte copies match independently; source hashes stay unchanged; browser and missing-media dismissal return to the shell. | [MEDIA results](../build/full/hdd-release-2026-09-06/media-final/result.json) |
| Final Windows 3.1, 800×600 | Two sessions on the final ISO's installed HDD, Calculator 2+3=5 with display-only OCR, resize/close with zero residual changed pixels, WAV/MIDI playback and DOS return. PCM RMS: 1178 / 716. | [800 result](../build/full/hdd-release-final-2026-09-06/windows-800/result.json), [Calculator display](../build/full/hdd-release-final-2026-09-06/windows-800/calculator-result-display.txt) |
| Windows 3.1, SAFE | Same two-session checks; zero residual changed pixels. WAV/MIDI PCM RMS: 1182 / 715. | [SAFE result](../build/full/hdd-release-2026-09-06/windows-safe-2/result.json), [Calculator display](../build/full/hdd-release-2026-09-06/windows-safe-2/calculator-result-display.txt) |

Windows 3.1 checks establish the exercised behavior. Windows 95 installation
or compatibility has not been qualified. The final rebuilt ISO has passed
fresh installation, all eight HDD/application cases and the Windows 800×600
run, with no runtime kernel, shell or application replacement.

## Media limits

MEDIA provides read-only browsing and copying from FAT12/16/32 and ISO 9660.
The desktop's Floppy, USB and CD icons open that reader. It does not mount
external media as ordinary DOS drive letters: copy files to the installed
filesystem for use by existing DOS applications. USB disks must be exposed by
firmware before boot; native USB host-controller and hot-plug support are not
implemented. Optical access covers BIOS optical services and legacy IDE ATAPI,
not a general AHCI/USB optical stack. See [formats and commands](removable-media.md).

## Artifact snapshot

Hashes recorded from these files at delivery. These are not hashes of a readback
from an optical disc.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `build/full/obj/ciukidos.sys` | 43195 | `7c482ad57a0bb44d3456fb0a5e999d13bbe5743468731a4e739f5186f671d1e3` |
| `build/full/obj/shell.com` | 49296 | `e5d54d2e25f67fa917cba0d22f66044eb0faf2cd394f84ca94d9ed5e32aaa700` |
| `build/full/CiukiOS_full_cd_0-7-1.iso` | 149364736 | `8b7a3971d6d509d3c94808b157de4c0fb1f44ab20ce470273c9cb9ecd27c3458` |
| `build/full/ciukios-full-cd-disk.img` | 100695552 | `c52141fb75e579ab32ebc1d2e88ead5fb7d5338d8cac7059c2e59b04279f9c67` |

Before the final clock-only shell correction, both setup installation methods
produced the same 128 MiB HDD image:
`build/full/hdd-release-2026-09-06/install-quick/target.img`, SHA-256
`f2fecb3535941b1e30ef206ec553fcf6926105b8e5e5d2717e4ba95f9c0670ce`.
The earlier installed-acceptance and MEDIA checks used copies of this image
with its existing payloads. MEDIA.COM and the kernel are identical in the
final rebuild; only the native shell's clock correction changed afterward.

After the clock correction, the final ISO's quick installation produced
`build/full/hdd-release-final-2026-09-06/install-quick/target.img`, SHA-256
`181a59299cdb122e8f2f4436ae86de2bf596e4250ea63e7d626d13507e08ddca`.
The final application runs use disposable copies of this new installed HDD,
with no kernel, shell or application payload replacement.

## CD delivery

The final ISO above was written to the CD-RW in `/dev/sr0` with xorriso,
using fast blanking, DAO and eject. The writer reported successful completion
and a closed session; the process returned exit code 0. No separate optical
preflight or post-write readback was performed, as requested.
[Writer log](../build/full/hdd-release-final-2026-09-06/burn.log) and
[delivery record](../build/full/hdd-release-final-2026-09-06/burn-result.json).

The VS Code session retains an 8,000,000,000-byte hard memory limit and a
7,000,000,000-byte pressure threshold. Physical T23 testing remains distinct
from the installed-HDD emulator evidence above.
